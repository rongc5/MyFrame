#include "base_net_thread.h"
#include "channel_data_process.h"
#include "common_obj_container.h"
#include "base_net_obj.h"
#include "base_connect.h"
#include "common_util.h"
#include "factory_base.h"
#include "net_thread_context.h"
#include "obj_task_msg.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace {
thread_local uint32_t t_current_net_thread_index = 0;

// Upper bound on how long shutdown waits for queued messages and sends.
const uint64_t kShutdownDrainMs = 500;

class InflightGuard {
public:
    explicit InflightGuard(std::atomic<uint32_t> & c) : _c(c) {
        _c.fetch_add(1, std::memory_order_seq_cst);
    }
    ~InflightGuard() { _c.fetch_sub(1, std::memory_order_seq_cst); }
    InflightGuard(const InflightGuard &) = delete;
    InflightGuard & operator=(const InflightGuard &) = delete;
private:
    std::atomic<uint32_t> & _c;
};
}

namespace myframe {
namespace detail {

uint32_t current_net_thread_index()
{
    return t_current_net_thread_index;
}

NetThreadState net_thread_state(uint32_t thread_index)
{
    return base_net_thread::thread_state(thread_index);
}

NetThreadScope::NetThreadScope(uint32_t thread_index) : _prev(t_current_net_thread_index)
{
    t_current_net_thread_index = thread_index;
}

NetThreadScope::~NetThreadScope()
{
    t_current_net_thread_index = _prev;
}

} // namespace detail
} // namespace myframe

bool post_obj_task(const ObjId & id, uint32_t generation, obj_task_msg::task_fn fn)
{
    if (!fn) return false;
    std::shared_ptr<normal_msg> msg = std::make_shared<obj_task_msg>(generation, std::move(fn));
    return base_net_thread::post_obj_msg(id, std::move(msg));
}

base_net_thread::base_net_thread(int channel_num):_channel_num(channel_num), _base_container(NULL), _factory_for_thread(nullptr){
    net_thread_init();
}

base_net_thread::base_net_thread(IFactory* factory, int channel_num)
    : _channel_num(channel_num), _base_container(NULL), _factory_for_thread(factory) {
    net_thread_init();
}

void base_net_thread::unpublish_thread()
{
    const uint32_t idx = get_thread_index();
    if (idx < FAST_SLOT_COUNT) {
        _fast_slots[idx].store(nullptr, std::memory_order_seq_cst);
    }
    {
        std::lock_guard<std::mutex> lock(_thread_map_mutex);
        _base_net_thread_map.erase(idx);
    }
    if (idx < FAST_SLOT_COUNT) {
        while (_inflight[idx].count.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
    }
}

base_net_thread::~base_net_thread(){
    unpublish_thread();
    clear_user_data();
    // Container teardown destroys connections; mark this thread as their owner
    // so owner-thread-only operations execute inline instead of being posted.
    myframe::detail::NetThreadScope owner_scope(get_thread_index());
    // Drop these references first so each channel connection is destroyed by the
    // container while its epoll is valid, instead of later via a dangling pointer.
    _channel_msg_vec.clear();
    if (_base_container){
        delete _base_container;
    }
}

void * base_net_thread::run()
{
    myframe::detail::NetThreadScope owner_scope(get_thread_index());
    _loop_active.store(true, std::memory_order_release);
    while (get_run_flag()) {
        run_process();
        _base_container->obj_process();
        // 无条件排空各通道消息队列 (rxlogger::obj_process 模型): obj_process 里的
        // epoll_wait 带超时, 故本循环至少每个超时跑一遍; 这里每轮都 drain, 使跨线程
        // 消息的处理不依赖唤醒字节 —— 即便唤醒字节因 EPOLLET 边沿竞争/缓冲满而丢失,
        // 积压消息也会在一个超时内被处理。唤醒字节只负责"更快被唤醒"(降低延迟)。
        for (size_t i = 0; i < _channel_msg_vec.size(); ++i) {
            if (_channel_msg_vec[i]) {
                channel_data_process * cp = _channel_msg_vec[i]->process();
                if (cp) cp->on_tick();
            }
        }
        for (size_t i = 0; i < _plugins.size(); ++i) {
            if (_plugins[i]) _plugins[i]->on_tick();
        }
        // Connections that failed while handling a channel message are torn
        // down here, outside of any container map iteration.
        _base_container->flush_deferred_close();
    }
    drain_on_exit();
    _loop_active.store(false, std::memory_order_release);

    return NULL;
}

bool base_net_thread::stop()
{
    bool ret = base_thread::stop();
    for (size_t i = 0; i < _channel_msg_vec.size(); ++i) {
        if (_channel_msg_vec[i]) {
            channel_data_process * cp = _channel_msg_vec[i]->process();
            if (cp) cp->wake();
        }
    }
    return ret;
}

bool base_net_thread::drain_channels()
{
    bool pending = false;
    for (size_t i = 0; i < _channel_msg_vec.size(); ++i) {
        if (!_channel_msg_vec[i]) continue;
        channel_data_process * cp = _channel_msg_vec[i]->process();
        if (!cp) continue;
        try {
            cp->on_tick();
        } catch (std::exception & e) {
            fprintf(stderr, "[drain] channel handler threw: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[drain] channel handler threw\n");
        }
        if (cp->has_pending()) pending = true;
    }
    _base_container->flush_deferred_close();
    return pending;
}

void base_net_thread::drain_on_exit()
{
    const uint64_t deadline = GetMonotonicMilliSecond() + kShutdownDrainMs;
    auto drain_until_idle = [&]() {
        for (;;) {
            const bool queued = drain_channels();
            const bool unsent = _base_container->flush_outbound();
            if ((!queued && !unsent) || GetMonotonicMilliSecond() >= deadline) return;
            if (!queued) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // Phase 1: still accepting, so handlers may post follow-up messages.
    drain_until_idle();

    // Phase 2: reject new posts, wait for posters that passed the open check,
    // then drain what they queued. A successful post cannot be stranded.
    _loop_exited.store(true, std::memory_order_seq_cst);
    const uint32_t idx = get_thread_index();
    if (idx < FAST_SLOT_COUNT) {
        while (_inflight[idx].count.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
    } else {
        std::lock_guard<std::mutex> barrier(_thread_map_mutex);
    }
    drain_until_idle();
}

void base_net_thread::run_process()
{

}

void base_net_thread::net_thread_init()
{
    // Construction-time setup (factory hooks, listen sockets, channels) runs
    // before this thread is published; the constructing thread owns it.
    myframe::detail::NetThreadScope owner_scope(get_thread_index());
    _base_container = new common_obj_container(get_thread_index());
    _base_container->set_owner_thread(this);
    for (size_t i = 0; i < _plugins.size(); ++i) {
        if (_plugins[i]) _plugins[i]->on_init(this);
    }
    if (_factory_for_thread) {
        _factory_for_thread->net_thread_init(this);
    }

    for (int i = 0; i < _channel_num; i++) {

        int fd[2];
        int ret = socketpair(AF_UNIX,SOCK_STREAM,0,fd);
        if (ret < 0) {
            // Keep any channels already created. With none, put_msg is a no-op
            // and put_obj_msg reports failure.
            fprintf(stderr, "[net_thread] socketpair failed: %s\n", strError(errno).c_str());
            break;
        }

        std::shared_ptr<base_connect<channel_data_process> >  channel_connect(new base_connect<channel_data_process>(fd[0]));
        channel_data_process * data_process = new channel_data_process(channel_connect, fd[1]);
        channel_connect->set_process(data_process);
        channel_connect->set_net_container(_base_container);

        _channel_msg_vec.push_back(channel_connect);
    }


    {
        std::lock_guard<std::mutex> lock(_thread_map_mutex);
        _base_net_thread_map[get_thread_index()] = this;
    }
    // 发布到无锁快表 (map 先写, slot 后写; 读端 slot 命中即免锁, 未命中回退 map,
    // 故二者短暂不一致也安全)。
    uint32_t idx = get_thread_index();
    if (idx < FAST_SLOT_COUNT) {
        _fast_slots[idx].store(this, std::memory_order_seq_cst);
    }
}


void base_net_thread::put_msg(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg)
{
    // 按 obj_id 选通道 (而非 &p_msg 栈地址): 保证同一对象的消息恒落同一通道,
    // 不会跨通道乱序; 同时 obj_id 递增也给出均匀分布。
    if (_channel_msg_vec.empty()) {
        return;
    }
    int index = obj_id % _channel_msg_vec.size();
    std::shared_ptr<base_connect<channel_data_process> >  channel_connect = _channel_msg_vec[index];
    channel_data_process * data_process = channel_connect->process();
    if (data_process)
    {
        data_process->put_msg(obj_id, p_msg);
        //channel_connect->set_real_net(true); 风险
    }
}

void base_net_thread::handle_msg(std::shared_ptr<normal_msg> & p_msg)
{
    if (!p_msg) {
        return;
    }

    if (handle_thread_msg(p_msg)) {
        return;
    }

    if (_factory_for_thread) {
        _factory_for_thread->handle_thread_msg(p_msg);
    }
}

bool base_net_thread::handle_thread_msg(std::shared_ptr<normal_msg> &)
{
    return false;
}

base_net_thread * base_net_thread::resolve_thread(uint32_t thread_index)
{
    // 快表命中即无锁返回 (正常情况下所有线程都在表内)。
    if (thread_index < FAST_SLOT_COUNT) {
        base_net_thread * t = _fast_slots[thread_index].load(std::memory_order_acquire);
        if (t) {
            return t;
        }
        // slot 为空: 可能是注册期 map 已写 slot 未写的短暂窗口, 回退加锁 map。
    }
    std::lock_guard<std::mutex> lock(_thread_map_mutex);
    auto it = _base_net_thread_map.find(thread_index);
    return (it != _base_net_thread_map.end()) ? it->second : NULL;
}

base_net_thread * base_net_thread::get_base_net_thread_obj(uint32_t thread_index)
{
    return resolve_thread(thread_index);
}

void base_net_thread::add_timer(std::shared_ptr<timer_msg> & t_msg)
{
    _base_container->add_timer(t_msg);
}

bool base_net_thread::enqueue_if_open(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg)
{
    if (_channel_msg_vec.empty() || _loop_exited.load(std::memory_order_seq_cst)) {
        return false;
    }
    put_msg(obj_id, p_msg);
    return true;
}

bool base_net_thread::dispatch(const ObjId & id, std::shared_ptr<normal_msg> & p_msg)
{
    const uint32_t idx = id._thread_index;
    if (idx < FAST_SLOT_COUNT) {
        InflightGuard guard(_inflight[idx].count);
        base_net_thread * t = _fast_slots[idx].load(std::memory_order_seq_cst);
        if (t) {
            return t->enqueue_if_open(id._id, p_msg);
        }
    }
    // Slow path: index outside the fast table, or a registration/teardown
    // window. The destructor unregisters under this lock.
    std::lock_guard<std::mutex> lock(_thread_map_mutex);
    auto it = _base_net_thread_map.find(idx);
    if (it == _base_net_thread_map.end() || !it->second) {
        return false;
    }
    return it->second->enqueue_if_open(id._id, p_msg);
}

bool base_net_thread::put_obj_msg(ObjId & id, std::shared_ptr<normal_msg> & p_msg)
{
    return dispatch(id, p_msg);
}

bool base_net_thread::post_obj_msg(const ObjId & id, std::shared_ptr<normal_msg> p_msg)
{
    return dispatch(id, p_msg);
}

bool base_net_thread::is_loop_running(uint32_t thread_index)
{
    return thread_state(thread_index) == myframe::detail::NetThreadState::Running;
}

myframe::detail::NetThreadState base_net_thread::thread_state(uint32_t thread_index)
{
    auto state_of = [](const base_net_thread * t) {
        if (t->_loop_exited.load(std::memory_order_seq_cst)) {
            return myframe::detail::NetThreadState::Stopped;
        }
        return t->loop_active() ? myframe::detail::NetThreadState::Running
                                : myframe::detail::NetThreadState::Idle;
    };
    if (thread_index < FAST_SLOT_COUNT) {
        InflightGuard guard(_inflight[thread_index].count);
        base_net_thread * t = _fast_slots[thread_index].load(std::memory_order_seq_cst);
        if (t) return state_of(t);
    }
    std::lock_guard<std::mutex> lock(_thread_map_mutex);
    auto it = _base_net_thread_map.find(thread_index);
    if (it == _base_net_thread_map.end() || !it->second) {
        return myframe::detail::NetThreadState::Gone;
    }
    return state_of(it->second);
}

void base_net_thread::handle_timeout(std::shared_ptr<timer_msg> & t_msg)
{
    return ;
}

common_obj_container * base_net_thread::get_net_container()
{
    return _base_container;
}

void base_net_thread::set_user_data_unowned(const std::string& key, void* value)
{
    if (!value) {
        _user_data.erase(key);
        return;
    }
    store_user_data(key, std::shared_ptr<void>(value, [](void*) {}), false);
}

void base_net_thread::clear_user_data(const std::string& key)
{
    _user_data.erase(key);
}

void base_net_thread::clear_user_data()
{
    _user_data.clear();
}

void base_net_thread::store_user_data(const std::string& key, std::shared_ptr<void> value, bool owns)
{
    if (!value) {
        _user_data.erase(key);
        return;
    }
    ThreadUserDataEntry entry;
    entry.handle = std::move(value);
    entry.owns = owns;
    _user_data[key] = std::move(entry);
}

std::unordered_map<uint32_t, base_net_thread *> base_net_thread::_base_net_thread_map;
std::mutex base_net_thread::_thread_map_mutex;
std::atomic<base_net_thread *> base_net_thread::_fast_slots[base_net_thread::FAST_SLOT_COUNT];
base_net_thread::InflightSlot base_net_thread::_inflight[base_net_thread::FAST_SLOT_COUNT];

void base_net_thread::add_worker_thread(uint32_t thread_index)
{
    // 避免重复
    if (std::find(_worker_pool.begin(), _worker_pool.end(), thread_index) == _worker_pool.end()) {
        _worker_pool.push_back(thread_index);
    }
}

uint32_t base_net_thread::next_worker_rr()
{
    if (_worker_pool.empty()) return get_thread_index();
    unsigned long idx = _rr_hint.fetch_add(1, std::memory_order_relaxed);
    return _worker_pool[idx % _worker_pool.size()];
}

 
