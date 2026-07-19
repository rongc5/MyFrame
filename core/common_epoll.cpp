#include "common_epoll.h"
#include "base_net_obj.h"
#include "common_obj_container.h"
#include "common_exception.h"

// epoll event payload = (generation << 32) | obj_id, 让 epoll_wait 回查 container.
// 永远不解引用 epoll_event.data.ptr 那种 raw pointer, 避免 use-after-free。
static inline uint64_t pack_epoll_payload(base_net_obj * obj) {
    return ((uint64_t)obj->generation() << 32)
         | (uint64_t)(uint32_t)obj->get_id()._id;
}



void common_epoll::add_to_epoll(base_net_obj * p_obj)
{		
    int tmpOprate = EPOLL_CTL_ADD;
    struct epoll_event tmpEvent;
    memset(&tmpEvent, 0, sizeof(epoll_event));
    tmpEvent.events = p_obj->get_event();
    tmpEvent.data.u64 = pack_epoll_payload(p_obj);
    int ret = epoll_ctl(_epoll_fd, tmpOprate, p_obj->get_sfd(), &tmpEvent);
    PDEBUG("add to epoll _epoll_fd[%d] _get_sock [%d]", _epoll_fd, p_obj->get_sfd());
    if (ret != 0) {
        int e = errno;
        if (e == EEXIST) {
            // Already registered on this epoll; update events instead of failing
            tmpOprate = EPOLL_CTL_MOD;
            int r2 = epoll_ctl(_epoll_fd, tmpOprate, p_obj->get_sfd(), &tmpEvent);
            if (r2 == 0) return;
            std::string err2 = strError(errno);
            PDEBUG("epoll MOD after EEXIST failed: %s", err2.c_str());
        }
        std::string err = strError(e);
        PDEBUG("add to epoll fail %s", err.c_str());
        THROW_COMMON_EXCEPT("add to epoll fail " << err);
    }
}

void common_epoll::del_from_epoll(base_net_obj * p_obj)
{
    int tmpOprate = EPOLL_CTL_DEL;
    struct epoll_event tmpEvent;
    memset(&tmpEvent, 0, sizeof(epoll_event));
    tmpEvent.data.u64 = pack_epoll_payload(p_obj);;
    int ret = epoll_ctl(_epoll_fd, tmpOprate, p_obj->get_sfd(), &tmpEvent);
    PDEBUG("delete to epoll _epoll_fd[%d] _get_sock [%d]", _epoll_fd, p_obj->get_sfd());
    if (ret != 0) {
        if (errno == ENOENT) {
            // Not present; treat as success to be robust against races
            return;
        }
        THROW_COMMON_EXCEPT("del from epoll fail " << strError(errno).c_str());
    }
}

void common_epoll::mod_from_epoll(base_net_obj * p_obj)
{
    int tmpOprate = EPOLL_CTL_MOD;
    struct epoll_event tmpEvent;
    tmpEvent.events =  p_obj->get_event();  
    tmpEvent.data.u64 = pack_epoll_payload(p_obj);
    int ret = epoll_ctl(_epoll_fd, tmpOprate, p_obj->get_sfd(), &tmpEvent);
    if (ret != 0) {
        if (errno == ENOENT) {
            // Not present; try add instead
            tmpOprate = EPOLL_CTL_ADD;
            int r2 = epoll_ctl(_epoll_fd, tmpOprate, p_obj->get_sfd(), &tmpEvent);
            if (r2 == 0) return;
        }
        THROW_COMMON_EXCEPT("mod from epoll fail "<< strError(errno).c_str());
    }
}

int common_epoll::epoll_wait(std::map<ObjId, std::shared_ptr<base_net_obj> > &expect_list, std::map<ObjId, std::shared_ptr<base_net_obj> > &remove_list, uint32_t num)
{
    int  nfds = ::epoll_wait(_epoll_fd, _epoll_events, _epoll_size, _epoll_wait_time);
    if (nfds == -1)
    {
        std::string err = strError(errno);
        PDEBUG("epoll_wait fail [%s]", err.c_str());          
        if (errno == EINTR)
            return 0;
        THROW_COMMON_EXCEPT("epoll_wait fail "<< err);          
        //return 0;
    }      

    // epoll event payload = (generation<<32) | obj_id. 不再解引用 data.ptr
    // 那种 raw pointer — 所有 event 经 container 反查活对象, 查不到丢弃。
    // 这彻底消除以前 "dangling pointer 在 epoll 注册表里残留 → next epoll_wait
    // 返回这个指针 → shared_from_this() 触发 bad_weak_ptr 或 0x10 segfault" 的
    // UAF 类崩溃。代价: 每事件多一次 unordered_map find (~50ns), 跟 syscall 比
    // 是 noise。
    //
    // 注: _owner_container 由 ~common_obj_container 在 init() 之后 attach,
    // 理论上不可能 nullptr; defensive check 仅防 stale lib 链接的边界。
    for (int i = 0; i < nfds; i++)
    {
        const uint64_t payload = _epoll_events[i].data.u64;
        const uint32_t obj_id  = (uint32_t)(payload & 0xFFFFFFFFu);
        const uint32_t gen     = (uint32_t)(payload >> 32);

        if (obj_id == 0 || _owner_container == NULL) {
            // 旧版兼容残留 / 未 attach container — drop event.
            continue;
        }

        std::shared_ptr<base_net_obj> p_obj = _owner_container->find(obj_id);
        if (!p_obj) {
            // obj 已死, 还没从 epoll 摘掉; 静默丢弃。
            continue;
        }
        if (p_obj->generation() != gen) {
            // obj_id 被 reuse 给新对象 (老对象已死), 这个 event 是给老对象的, drop.
            fprintf(stderr,
                    "[epoll_wait] stale event: obj_id=%u gen=%u live_gen=%u, skip\n",
                    obj_id, gen, p_obj->generation());
            continue;
        }

        try
        {
#ifdef EPOLLRDHUP
            if ((_epoll_events[i].events & (EPOLLERR|EPOLLHUP|EPOLLRDHUP)) != 0) {
                PDEBUG("[NOTICE] epoll fd[%d] events=0x%x (ERR/HUP)",
                       p_obj->get_sfd(), _epoll_events[i].events);
            }
#else
            if ((_epoll_events[i].events & (EPOLLERR|EPOLLHUP)) != 0) {
                PDEBUG("[NOTICE] epoll fd[%d] events=0x%x (ERR/HUP)",
                       p_obj->get_sfd(), _epoll_events[i].events);
            }
#endif
            p_obj->event_process(_epoll_events[i].events);
            if (p_obj->get_real_net()) {
                remove_list.insert(std::make_pair(p_obj->get_id(), p_obj));
            }
        }
        catch (CMyCommonException &e)
        {
            fprintf(stderr, "[epoll_wait] CMyCommonException obj_id=%d fd=%d: %s\n",
                    p_obj->get_id()._id, p_obj->get_sfd(), e.what());
            expect_list.insert(std::make_pair(p_obj->get_id(), p_obj));
        }
        catch (std::exception &e)
        {
            fprintf(stderr, "[epoll_wait] std::exception obj_id=%d fd=%d: %s\n",
                    p_obj->get_id()._id, p_obj->get_sfd(), e.what());
            expect_list.insert(std::make_pair(p_obj->get_id(), p_obj));
        }
    }
    return nfds;
}
