#include "common_def.h"
#include "common_epoll.h"
#include "common_obj_container.h"
#include "base_net_thread.h"
#include "base_timer.h"

#include "common_util.h"
#include "common_domain.h"
#include "net_thread_context.h"
#include "obj_task_msg.h"

namespace {
bool is_expected_disconnect(const char* message) {
    if (!message) return false;
    return strstr(message, "peer closed") != NULL ||
           strstr(message, "client close the socket") != NULL ||
           strstr(message, "delay close") != NULL ||
           strstr(message, "Connection reset by peer") != NULL ||
           strstr(message, "Broken pipe") != NULL;
}
}

common_obj_container::common_obj_container(uint32_t thread_index, uint32_t epoll_size)
{
    _p_epoll = new common_epoll();
    _p_epoll->init(epoll_size);
    // 反向引用: 给 epoll_wait 用 obj_id->shared_ptr 查活对象, 不再解引用 raw pointer.
    _p_epoll->attach_container(this);

    _timer = new base_timer(this);

    _domain = new common_domain(this);

    _id_str._id = OBJ_ID_BEGIN;
    _id_str._thread_index = thread_index;
}

common_obj_container::~common_obj_container()
{
    myframe::detail::NetThreadScope owner_scope(_id_str._thread_index);
    _deferred_close.clear();
    for (const auto & k :_obj_map)
    {
        k.second->destroy();
    }

    // 强制 drop container 持有的 shared_ptr<base_net_obj> refs, 让真正的
    // ~base_net_obj() 在这里跑 — 它内部会调 common_epoll::del_from_epoll(_p_epoll),
    // 必须在 _p_epoll 还有效时完成。real-net 对象同时存在于 _obj_map 和
    // _obj_net_map, 两个 map 都必须在 delete _p_epoll 前清掉。
    //
    // 修复前的 UAF: clear() 没调 → 函数体结束后 C++ 才析构 map 成员, 那时
    // _p_epoll 已 delete → del_from_epoll 读 freed memory → 堆 metadata 被踩花。
    // valgrind 2026-05-27 锁定; 触发路径: DictUpdateThread::httpGet 每 60s 一次。
    _obj_net_map.clear();
    _obj_map.clear();

    if (_p_epoll != NULL)
        delete _p_epoll;

    if (_timer)
        delete _timer;

    if (_domain)
        delete _domain;
}


common_epoll * common_obj_container::get_epoll()
{
        return _p_epoll;
}

base_timer * common_obj_container::get_timer()
{
        return _timer;
}

const ObjId & common_obj_container::gen_id_str()
{
    do {
        if (_id_str._id <= OBJ_ID_BEGIN)
            _id_str._id = OBJ_ID_BEGIN;

        _id_str._id++;
    } while (find(_id_str._id));

    return _id_str;
}


bool common_obj_container::push_real_net(std::shared_ptr<base_net_obj> & p_obj)
{
    PDEBUG("base_net_obj:%p, .use_count:%ld, _id:%d _thread_index:%d", (void*)p_obj.get(), (long)p_obj.use_count(), p_obj->get_id()._id, p_obj->get_id()._thread_index);

    auto it = _obj_net_map.find(p_obj->get_id()._id);
    if (it == _obj_net_map.end())
    {
        _obj_net_map.insert(std::make_pair(p_obj->get_id()._id, p_obj));
    }
    //_obj_net_map[p_obj->get_id()._id] = p_obj;

    PDEBUG("base_net_obj:%p, .use_count:%ld, _id:%d _thread_index:%d", (void*)p_obj.get(), (long)p_obj.use_count(), p_obj->get_id()._id, p_obj->get_id()._thread_index);

    return true;
}

bool common_obj_container::remove_real_net(std::shared_ptr<base_net_obj> & p_obj)
{
    PDEBUG("base_net_obj:%p, .use_count:%ld, _id:%d _thread_index:%d", (void*)p_obj.get(), (long)p_obj.use_count(), p_obj->get_id()._id, p_obj->get_id()._thread_index);
    _obj_net_map.erase(p_obj->get_id()._id);
    PDEBUG("base_net_obj:%p, .use_count:%ld, _id:%d _thread_index:%d", (void*)p_obj.get(), (long)p_obj.use_count(), p_obj->get_id()._id, p_obj->get_id()._thread_index);

    return true;
}

uint32_t common_obj_container::size()
{
    return _obj_map.size();
}

bool common_obj_container::insert(std::shared_ptr<base_net_obj> &p_obj)
{
    p_obj->set_id(gen_id_str());

    _obj_map[p_obj->get_id()._id] = p_obj;

    return true;
}

uint32_t common_obj_container::get_thread_index()
{
    return _id_str._thread_index;
}

void common_obj_container::handle_timeout(std::shared_ptr<timer_msg> & t_msg)
{
    switch (t_msg->_obj_id)
    {
        case OBJ_ID_THREAD:
            {
                if (_owner_thread)
                {
                    _owner_thread->handle_timeout(t_msg);
                }
            }
            break;
        case OBJ_ID_DOMAIN:
            {
                if (_domain)
                    _domain->handle_timeout(t_msg);
            }
            break;
        default:
            {
                std::shared_ptr<base_net_obj>  net_obj = find(t_msg->_obj_id);
                if (net_obj)
                    net_obj->handle_timeout(t_msg);
            }
    }
    //OBJ_ID_DOMAIN

}

void common_obj_container::add_timer(std::shared_ptr<timer_msg> & t_msg)
{
    if (_timer)
    {
        _timer->add_timer(t_msg); 
    }
}

common_domain * common_obj_container::get_domain()
{
    return _domain;
}

std::shared_ptr<base_net_obj> common_obj_container::find(uint32_t obj_id)
{
    auto it = _obj_map.find(obj_id);
    if (it == _obj_map.end()){
        return NULL;
    }else {
        return it->second;
    }
}

void common_obj_container::erase(uint32_t obj_id)
{
    _obj_net_map.erase(obj_id);
    _obj_map.erase(obj_id);

    return;
}

void common_obj_container::handle_msg(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg)
{
    if (!p_msg) {
        return;
    }
    switch (obj_id)
    {
        case OBJ_ID_THREAD:
            {
                if (_owner_thread)
                {
                    try {
                        _owner_thread->handle_msg(p_msg);
                    } catch (std::exception &e) {
                        fprintf(stderr, "[handle_msg] thread message handler threw: %s\n", e.what());
                    } catch (...) {
                        fprintf(stderr, "[handle_msg] thread message handler threw\n");
                    }
                }
            }
            break;
        default:
            {
                std::shared_ptr<base_net_obj>  net_obj = find(obj_id);
                if (!net_obj) {
                    // Connection already closed: drop the message.
                    return;
                }
                try {
                    if (p_msg->_msg_op == NORMAL_MSG_OBJ_TASK) {
                        obj_task_msg * task = dynamic_cast<obj_task_msg *>(p_msg.get());
                        if (task) {
                            // A recycled obj_id must never receive a task that
                            // was addressed to an earlier connection.
                            if (task->_generation != 0 && task->_generation != net_obj->generation()) {
                                return;
                            }
                            if (task->_fn) task->_fn(*net_obj);
                            return;
                        }
                    }
                    net_obj->handle_msg(p_msg);
                } catch (CMyCommonException &e) {
                    if (!is_expected_disconnect(e.what())) {
                        PDEBUG("[handle_msg] obj_id=%u: %s", obj_id, e.what());
                    }
                    defer_close(net_obj);
                } catch (std::exception &e) {
                    fprintf(stderr, "[handle_msg] std::exception obj_id=%u: %s\n", obj_id, e.what());
                    defer_close(net_obj);
                }
            }
    }
}

void common_obj_container::defer_close(const std::shared_ptr<base_net_obj> & p_obj)
{
    if (!p_obj) return;
    for (const auto & o : _deferred_close) {
        if (o == p_obj) return;
    }
    if (_p_epoll) {
        try { _p_epoll->del_from_epoll(p_obj.get()); } catch (...) {}
    }
    try { p_obj->destroy(); } catch (...) {}
    _deferred_close.push_back(p_obj);
}

void common_obj_container::flush_deferred_close()
{
    if (_deferred_close.empty()) return;
    std::vector<std::shared_ptr<base_net_obj> > pending;
    pending.swap(_deferred_close);
    for (auto & obj : pending) {
        uint32_t id = obj->get_id()._id;
        auto it = _obj_map.find(id);
        // Only erase the exact object that failed; the id may have been reused.
        if (it != _obj_map.end() && it->second == obj) {
            erase(id);
        }
    }
}

bool common_obj_container::flush_outbound()
{
    std::vector<std::shared_ptr<base_net_obj> > targets;
    for (const auto & kv : _obj_map) {
        if (kv.second && kv.second->wants_tick()) targets.push_back(kv.second);
    }
    bool pending = false;
    for (auto & obj : targets) {
        try {
            if (obj->flush_outbound()) pending = true;
        } catch (CMyCommonException &) {
            defer_close(obj);
        } catch (std::exception &) {
            defer_close(obj);
        }
    }
    flush_deferred_close();
    return pending;
}

void common_obj_container::obj_process()
{   
    myframe::detail::NetThreadScope owner_scope(_id_str._thread_index);
    flush_deferred_close();
    std::vector<std::shared_ptr<base_net_obj> > exception_vec;
    std::vector<std::shared_ptr<base_net_obj> > real_net_vec;

    // Snapshot the real-net objects first: a handler may insert into (or
    // erase from) _obj_net_map during real_net_process(), and a rehash would
    // invalidate a live iterator. Objects added during this tick are picked
    // up on the next one.
    std::vector<std::shared_ptr<base_net_obj> > tick_objs;
    tick_objs.reserve(_obj_net_map.size());
    for (std::unordered_map<uint32_t, std::shared_ptr<base_net_obj> >::const_iterator it = _obj_net_map.begin();
         it != _obj_net_map.end(); ++it)
    {
        if (it->second) tick_objs.push_back(it->second);
    }

    for (size_t i = 0; i < tick_objs.size(); ++i)
    {
        const std::shared_ptr<base_net_obj> & obj = tick_objs[i];
        // Skip objects removed earlier in this tick (or whose id was reused).
        std::unordered_map<uint32_t, std::shared_ptr<base_net_obj> >::const_iterator cur =
            _obj_net_map.find(obj->get_id()._id);
        if (cur == _obj_net_map.end() || cur->second != obj) {
            continue;
        }
        try
        {
            obj->real_net_process();
            if (!obj->get_real_net()) {
                real_net_vec.push_back(obj);
            }
        }
        catch(CMyCommonException &e)
        {
            PDEBUG("CMyCommonException obj_id=%d: %s", obj->get_id()._id, e.what());
            if (!is_expected_disconnect(e.what())) {
                fprintf(stderr, "[obj_process] CMyCommonException obj_id=%d: %s\n",
                        obj->get_id()._id, e.what());
            }
            exception_vec.push_back(obj);
        }
        catch(std::exception &e)
        {
            PDEBUG("std::exception obj_id=%d: %s", obj->get_id()._id, e.what());
            fprintf(stderr, "[obj_process] std::exception obj_id=%d: %s\n",
                    obj->get_id()._id, e.what());
            exception_vec.push_back(obj);
        }
    }

    for (std::vector<std::shared_ptr<base_net_obj> >::iterator tmp_itr = real_net_vec.begin(); 
            tmp_itr != real_net_vec.end(); tmp_itr++) {
        PDEBUG("remove_real_net: _id:%d, _thread_index:%d", (*tmp_itr)->get_id()._id, (*tmp_itr)->get_id()._thread_index);            
        remove_real_net(*tmp_itr);
    }

    auto detach_from_epoll = [](const std::shared_ptr<base_net_obj>& obj) {
        if (!obj) return;
        if (auto* container = obj->get_net_container()) {
            if (auto* ep = container->get_epoll()) {
                try {
                    ep->del_from_epoll(obj.get());
                } catch (...) {
                    // best effort during teardown
                }
            }
        }
    };

    for (auto& obj : exception_vec) {
        detach_from_epoll(obj);
        obj->destroy();
        erase(obj->get_id()._id);
    }

    std::map<ObjId, std::shared_ptr<base_net_obj> > exp_list;
    std::map<ObjId, std::shared_ptr<base_net_obj> > remove_list;

    _p_epoll->epoll_wait(exp_list, remove_list, static_cast<uint32_t>(_obj_net_map.size()));
    for (std::map<ObjId, std::shared_ptr<base_net_obj> >::iterator itr = exp_list.begin(); itr != exp_list.end(); ++itr)
    {         	
        PDEBUG("step2: _id:%d, _thread_index:%d", itr->second->get_id()._id, itr->second->get_id()._thread_index);            
        detach_from_epoll(itr->second);
        itr->second->destroy();
        erase(itr->first._id);
    }

    for (std::map<ObjId, std::shared_ptr<base_net_obj> >::iterator itr = remove_list.begin(); itr != remove_list.end(); ++itr)
    {
        PDEBUG("remove_real_net: _id:%d, _thread_index:%d", itr->second->get_id()._id, itr->second->get_id()._thread_index);            
        itr->second->set_real_net(false);
        remove_real_net(itr->second);
    }

    std::vector<uint32_t> exp_vec;

    get_timer()->check_timer(exp_vec);

    for (auto it = exp_vec.begin(); it != exp_vec.end(); it++)
    {
        std::shared_ptr<base_net_obj>  net_obj = find(*it);
        if (net_obj)
        {
            net_obj->destroy();
            erase(*it);
        }
    }

    flush_deferred_close();
}
