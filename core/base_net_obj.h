#ifndef __BASE_NET_OBJ_H__
#define __BASE_NET_OBJ_H__

#include "common_util.h"
#include <atomic>
#include <string>
#include <functional>

class base_data_process;
class common_obj_container;
class base_net_obj: public std::enable_shared_from_this<base_net_obj>
{
    public:
        base_net_obj();

        virtual ~base_net_obj();

        // 进程级单调递增 generation, ctor 时一次性分配, 终身不变。
        // 用途: epoll event payload = (generation<<32) | obj_id, 让 epoll_wait
        // 拿到 event 后能 (a) 查 container 看对象是否还活着 (b) 比对 generation
        // 防止 obj_id 被复用给新对象时把旧事件投递到新对象上。
        uint32_t generation() const { return _generation; }

        virtual void event_process(int events) = 0;

        bool get_real_net();
        void set_real_net(bool real_net);

        virtual int real_net_process()=0;

        virtual void set_net_container(common_obj_container *p_net_container);

        common_obj_container * get_net_container();

        virtual void update_event(int event);    
        int get_event();

        virtual void notice_send();

        int get_sfd();

        // Closed / not-yet-connected sentinel for _fd. fd 0 is a valid
        // descriptor and must never be mistaken for "no socket".
        static constexpr int kInvalidFd = -1;
        bool has_valid_fd() const { return _fd >= 0; }

        // True when the calling thread may touch this connection's socket and
        // send state directly: it is the connection's owner network thread,
        // the connection is not attached to a running event loop (setup /
        // teardown), or it has no container yet. Any other caller must post
        // the work to the owner thread (see obj_task_msg.h).
        bool on_owner_thread() const;

        // Post fn to this connection's owner thread. The closure only runs if
        // this exact connection (same generation) is still registered.
        bool post_to_owner(std::function<void(base_net_obj &)> fn);

        // Ask the owner thread to run notice_send(); repeated foreign requests
        // are coalesced until the owner has processed the pending one.
        bool post_notice_send_to_owner();

        // Ask the owner thread to tear this connection down through the
        // standard container path (destroy + erase).
        bool post_close_to_owner(const char * reason);

        void set_id(const ObjId & id_str);
        const ObjId & get_id();

        virtual void handle_msg(std::shared_ptr<normal_msg> & p_msg);

        void add_timer(std::shared_ptr<timer_msg> & t_msg);
        virtual void handle_timeout(std::shared_ptr<timer_msg> & t_msg);

        virtual void destroy();

        // Activity hint for idle-skip. Updated on IO and event processing.
        void touch_active(uint64_t now_ms);
        uint64_t last_active_ms() const { return _last_active_ms; }

        // Hint for the container whether this object wants a tick outside of
        // epoll events (e.g., TLS handshake progress or pending writes).
        // Default: when EPOLLOUT is armed.
        virtual bool wants_tick() const { return (_epoll_event & EPOLLOUT) == EPOLLOUT; }
        // Owner thread only: try to write queued data without reading. Returns
        // true while outbound data remains. Used by the shutdown drain.
        virtual bool flush_outbound() { return false; }

        net_addr & get_peer_addr();

        // Bind a resolved protocol tag to this connection. When lock is true,
        // callers promise the protocol will not change for the lifetime of the
        // connection.
        void set_protocol_tag(const std::string& tag, bool lock = true);
        const std::string& protocol_tag() const { return _protocol_tag; }
        bool protocol_locked() const { return _protocol_locked; }
        void clear_protocol_tag();

    protected:
        void add_timer();

    protected:
        common_obj_container *_p_net_container;
        int _epoll_event;
        int _fd;	
        ObjId _id_str;
        bool _real_net;
        std::vector<std::shared_ptr<timer_msg> > _timer_vec;

        net_addr _peer_net;
        uint64_t _last_active_ms{0};
        std::string _protocol_tag;
        bool _protocol_locked{false};
        uint32_t _generation{0};
        // Coalesces notice_send() requests posted from foreign threads.
        std::atomic<bool> _foreign_notice_pending{false};
        // Whether the container this object lives in belongs to a network
        // thread (captured once in set_net_container on the setup thread).
        bool _owner_bound{false};
};



#endif
