#ifndef __BASE_NET_OBJ_H__
#define __BASE_NET_OBJ_H__

#include "common_util.h"
#include <string>

class base_data_process;
class common_obj_container;
class base_net_obj: public std::enable_shared_from_this<base_net_obj>
{
    public:
        base_net_obj();

        virtual ~base_net_obj();

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
};



#endif
