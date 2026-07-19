#include "base_net_thread.h"
#include "channel_data_process.h"
#include "common_obj_container.h"
#include "base_net_obj.h"
#include "base_connect.h"
#include "common_util.h"
#include "factory_base.h"
#include <algorithm>

base_net_thread::base_net_thread(int channel_num):_channel_num(channel_num), _base_container(NULL), _factory_for_thread(nullptr){
    net_thread_init();
}

base_net_thread::base_net_thread(IFactory* factory, int channel_num)
    : _channel_num(channel_num), _base_container(NULL), _factory_for_thread(factory) {
    net_thread_init();
}

base_net_thread::~base_net_thread(){
    {
        std::lock_guard<std::mutex> lock(_thread_map_mutex);
        _base_net_thread_map.erase(get_thread_index());
    }
    uint32_t idx = get_thread_index();
    if (idx < FAST_SLOT_COUNT) {
        _fast_slots[idx].store(nullptr, std::memory_order_release);
    }
    clear_user_data();
    if (_base_container){
        delete _base_container;
    }
}

void * base_net_thread::run()
{
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
    }

    return NULL;
}

void base_net_thread::run_process()
{

}

void base_net_thread::net_thread_init()
{
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
            return ;
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
        _fast_slots[idx].store(this, std::memory_order_release);
    }
}


void base_net_thread::put_msg(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg)
{
    // 按 obj_id 选通道 (而非 &p_msg 栈地址): 保证同一对象的消息恒落同一通道,
    // 不会跨通道乱序; 同时 obj_id 递增也给出均匀分布。
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

void base_net_thread::put_obj_msg(ObjId & id, std::shared_ptr<normal_msg> & p_msg)
{
    // 生命周期保护: 全程持 _thread_map_mutex 调 put_msg。
    // ~base_net_thread() 也在该锁下从 map/fast_slot 摘除自己 (见析构), 故只要这里持锁,
    // 目标线程对象就不会在 put_msg(访问其 _base_container / _channel_msg_vec) 期间被析构。
    //
    // 之前的"无锁 resolve_thread 返回裸指针, 锁外 put_msg"有 use-after-free 窗口:
    // resolve 后、put_msg 前目标线程若析构, 即访问已释放对象。fast_slot 只是个原子裸
    // 指针, 不持有所有权, 挡不住并发析构。这里改回持锁覆盖 lookup+put_msg (旧框架行为)。
    // 代价: 跨线程投递串行在该锁上 —— 对本项目的消息频率可接受, 正确性优先。
    std::lock_guard<std::mutex> lock(_thread_map_mutex);
    auto it = _base_net_thread_map.find(id._thread_index);
    if (it != _base_net_thread_map.end() && it->second) {
        it->second->put_msg(id._id, p_msg);
    }
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
    unsigned long idx = _rr_hint++;
    return _worker_pool[idx % _worker_pool.size()];
}

 
