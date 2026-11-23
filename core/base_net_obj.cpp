#include "base_net_obj.h"
#include "common_epoll.h"
#include "common_obj_container.h"
#include "common_def.h"
#include "common_exception.h"

#include "base_data_process.h"
#include "common_util.h"
#include "net_thread_context.h"
#include "obj_task_msg.h"
#include "common_obj_container.h"

#include <atomic>

// 进程级单调 generation counter. 每个 base_net_obj ctor 取一个新值。
// 跟 ObjId 不同: ObjId 由 container::gen_id_str() 分配并可在 erase 后**复用**
// 给新对象, 但 generation 永远不复用。
// pre-increment 从 1 开始 (0 保留作 "未初始化" 哨兵).
static std::atomic<uint32_t> g_obj_generation_counter{0};

base_net_obj::base_net_obj()
{
    _fd = kInvalidFd;
    // Monitor read, hangups, and remote half-close by default
    _epoll_event = EPOLLIN | EPOLLERR | EPOLLHUP
#ifdef EPOLLRDHUP
        | EPOLLRDHUP
#endif
        ;
    _p_net_container = NULL;
    _real_net = false;
    _last_active_ms = GetMonotonicMilliSecond();
    _generation = ++g_obj_generation_counter;  // 1-based, 永不为 0
}

base_net_obj::~base_net_obj()
{
    try {
        if (_p_net_container) {
            common_epoll * p_epoll = _p_net_container->get_epoll();
            if (p_epoll) {
                p_epoll->del_from_epoll(this);
            }
        }
    } catch (...) {
        // Best-effort cleanup during teardown; ignore epoll removal errors
    }

    _epoll_event = 0;

    if (_fd >= 0) {
        ::close(_fd);
        PDEBUG("close %d", _fd);
        _fd = kInvalidFd;
    }
}

void base_net_obj::set_net_container(common_obj_container *p_net_container)
{
    _p_net_container = p_net_container;
    _owner_bound = (p_net_container && p_net_container->get_owner_thread() != NULL);
    common_epoll * p_epoll = _p_net_container->get_epoll();

    std::shared_ptr<base_net_obj> p=std::dynamic_pointer_cast<base_net_obj>(shared_from_this());

    try {
        // 必须先 insert (它内部调 set_id 分配 obj_id), 后 add_to_epoll —
        // 因为 add_to_epoll 把 (generation<<32) | obj_id 存到 epoll_event.data.u64,
        // 给 epoll_wait 回查 container 用; 顺序倒了 obj_id 还是 0, 查不到。
        // 副作用红利: 如果 add_to_epoll 失败, obj 还在 container 里, 析构时正常
        // 走 ~base_net_obj 清理 — 不会留 "在 epoll 里但不在 container 里" 的孤儿。
        _p_net_container->insert(p);
        if (_fd >= 0) {
            p_epoll->add_to_epoll(p.get());
        }
        add_timer();
    }
    catch (std::exception &e)
    {
        PDEBUG("%s", e.what());
    }
    catch (...)
    {
    }
}

bool base_net_obj::get_real_net()
{
    return _real_net;
}

void base_net_obj::set_real_net(bool real_net)
{
    
    _real_net = real_net;
    if (_real_net) {
    std::shared_ptr<base_net_obj> p=std::dynamic_pointer_cast<base_net_obj>(shared_from_this());
        //PDEBUG("base_net_obj:%d, .use_count:%d, _id:%d, _thread_index:%d", p, p.use_count(), p->get_id()._id, p->get_id()._thread_index);
        PDEBUG(".use_count:%ld, _id:%d, _thread_index:%d", (long)p.use_count(), p->get_id()._id, p->get_id()._thread_index);
        _p_net_container->push_real_net(p);
        //PDEBUG("base_net_obj:%d, .use_count:%d, _id:%d, _thread_index:%d", p, p.use_count(), p->get_id()._id, p->get_id()._thread_index);
        PDEBUG(".use_count:%ld, _id:%d, _thread_index:%d", (long)p.use_count(), p->get_id()._id, p->get_id()._thread_index);
    }
}

common_obj_container * base_net_obj::get_net_container()
{
    return _p_net_container;
}

void base_net_obj::update_event(int event)
{
    if (!_p_net_container)
    {
        return;
    }

    if (!on_owner_thread()) {
        // _epoll_event and the epoll registration belong to the owner thread.
        post_to_owner([event](base_net_obj & o) { o.update_event(event); });
        return;
    }

    if (_fd < 0) {
        // Closed socket: remember the mask but never touch epoll with it.
        _epoll_event = event;
        return;
    }

    common_epoll * p_epoll = _p_net_container->get_epoll();
    if (_epoll_event != event && p_epoll) {
        _epoll_event = event;
        p_epoll->mod_from_epoll(this);
    }
}

int base_net_obj::get_event()
{
    return _epoll_event;
}

int base_net_obj::get_sfd()
{
    return _fd;
}

bool base_net_obj::on_owner_thread() const
{
    if (!_p_net_container) {
        return true;
    }
    const uint32_t owner = _id_str._thread_index;
    const uint32_t cur = myframe::detail::current_net_thread_index();
    if (cur == owner) {
        return true;
    }
    if (cur != 0) {
        // Another network thread.
        return false;
    }
    if (!_owner_bound) {
        // Stand-alone container driven by the caller (unit tests).
        return true;
    }
    // Idle: setup before the loop starts, no concurrent owner. Stopped/Gone:
    // the container may be in teardown, so the caller must post (and the post
    // is dropped with the channel queue).
    return myframe::detail::net_thread_state(owner) == myframe::detail::NetThreadState::Idle;
}

bool base_net_obj::post_to_owner(std::function<void(base_net_obj &)> fn)
{
    return post_obj_task(_id_str, _generation, std::move(fn));
}

bool base_net_obj::post_notice_send_to_owner()
{
    if (_foreign_notice_pending.exchange(true, std::memory_order_acq_rel)) {
        return true;  // already queued
    }
    bool ok = post_to_owner([](base_net_obj & o) {
        o._foreign_notice_pending.store(false, std::memory_order_release);
        o.notice_send();
    });
    if (!ok) {
        _foreign_notice_pending.store(false, std::memory_order_release);
    }
    return ok;
}

bool base_net_obj::post_close_to_owner(const char * reason)
{
    std::string why = reason ? reason : "close requested";
    return post_to_owner([why](base_net_obj &) {
        THROW_COMMON_EXCEPT(why.c_str());
    });
}

void base_net_obj::set_id(const ObjId & id_str)
{
    _id_str = id_str;
}

const ObjId & base_net_obj::get_id()
{
    return _id_str;
}

void base_net_obj::handle_msg(std::shared_ptr<normal_msg> & p_msg)
{
}

void base_net_obj::notice_send()
{
}

void base_net_obj::destroy()
{
}

void base_net_obj::touch_active(uint64_t now_ms)
{
    _last_active_ms = now_ms;
}

void base_net_obj::add_timer(std::shared_ptr<timer_msg> & t_msg)
{
    if (_p_net_container)
    {
        _p_net_container->add_timer(t_msg);
        add_timer();
    }
    else
    {
        _timer_vec.push_back(t_msg);
    }
}

void base_net_obj::add_timer()
{
    std::vector<std::shared_ptr<timer_msg> >::iterator it;

    bool flag = false;
    for (it = _timer_vec.begin(); it != _timer_vec.end(); it++)
    {
        if (_p_net_container)
        {
            _p_net_container->add_timer(*it);
            flag = true;
        }
        else 
        {
            break;
        }
    }

    if (flag)
    {
        _timer_vec.clear();
    }
}

net_addr & base_net_obj::get_peer_addr()
{
    if (!_peer_net.ip.empty() && _peer_net.port)
    {
        return _peer_net;
    }

    struct sockaddr_in sa;
    socklen_t len = sizeof(struct sockaddr_in);

    if (!getpeername(_fd, (struct sockaddr *)&sa, &len))
    {
        _peer_net.ip = inet_ntoa(sa.sin_addr);
        _peer_net.port = ntohs(sa.sin_port);
    }

    return _peer_net;
}

void base_net_obj::set_protocol_tag(const std::string& tag, bool lock)
{
    _protocol_tag = tag;
    _protocol_locked = lock;
    PDEBUG("[net_obj] protocol tag set to '%s' (lock=%d)", _protocol_tag.c_str(), lock ? 1 : 0);
}

void base_net_obj::clear_protocol_tag()
{
    if (!_protocol_tag.empty()) {
        PDEBUG("[net_obj] clearing protocol tag '%s'", _protocol_tag.c_str());
    }
    _protocol_tag.clear();
    _protocol_locked = false;
}

void base_net_obj::handle_timeout(std::shared_ptr<timer_msg> & t_msg)
{
}
