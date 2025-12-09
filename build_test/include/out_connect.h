#ifndef __OUT_CONNECT_H__
#define __OUT_CONNECT_H__

#include "common_def.h"
#include "base_net_obj.h"
#include "common_obj_container.h"

#include "common_exception.h"
#include "common_epoll.h"
#include "base_connect.h"

enum CONNECT_STATUS
{
    CLOSED = 0,
    CONNECT_OK = 1,
    CONNECTING = 2  
};


template<class PROCESS>
class out_connect:public base_connect<PROCESS>
{
    public:

        out_connect(const std::string &ip, unsigned short port)
        {
            _status = CLOSED;
            _ip = ip;
            _port = port;
        }

        virtual ~out_connect()
        {
            _status = CLOSED;

        }

        void connect()
        {
            // Resolve host (supports domain/IP, IPv4/IPv6), attempt non-blocking connect
            struct addrinfo hints; memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
            hints.ai_socktype = SOCK_STREAM;
            struct addrinfo* res = nullptr;
            char portbuf[16]; snprintf(portbuf, sizeof(portbuf), "%hu", _port);
            int gai = getaddrinfo(_ip.c_str(), portbuf, &hints, &res);
            if (gai != 0 || !res)
            {
                THROW_COMMON_EXCEPT(std::string("getaddrinfo fail: ") + gai_strerror(gai));
            }

            int fd = -1; int last_err = 0; bool in_progress = false;
            for (auto p = res; p; p = p->ai_next)
            {
                fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (fd < 0) { last_err = errno; continue; }
                set_unblock(fd);
                int ret = ::connect(fd, p->ai_addr, p->ai_addrlen);
                if (ret == 0)
                {
                    // Connected immediately
                    in_progress = false; break;
                }
                if (errno == EINPROGRESS || errno == EALREADY)
                {
                    in_progress = true; break;
                }
                last_err = errno; ::close(fd); fd = -1;
            }
            freeaddrinfo(res);
            if (fd < 0)
            {
                THROW_COMMON_EXCEPT("connect fail: " << strError(last_err).c_str());
            }

            base_net_obj::_fd = fd;
            // Ensure we are registered/updated on epoll with writable interest
            this->update_event(this->get_event() | EPOLLOUT);
            _status = in_progress ? CONNECTING : CONNECT_OK;
            if (_status == CONNECT_OK)
            {
                connect_ok_process();
            }
        }

        // During non-blocking connect, we still must register/update
        // the socket with epoll (typically to watch EPOLLOUT for
        // connect completion). The previous gate suppressed updates
        // while CONNECTING, causing the fd to never be added to epoll
        // and the connection to hang forever.
        virtual void update_event(int event)
        {
            base_connect<PROCESS>::update_event(event);
        }

        virtual void event_process(int event)
        {
            if ((event & EPOLLERR) == EPOLLERR || (event & EPOLLHUP) == EPOLLHUP)
            {
                THROW_COMMON_EXCEPT("epoll error "<< strError(errno).c_str());
            }

            if (_status == CONNECTING)
            {
                // Robust completion check via SO_ERROR regardless of specific events
                int err = 0; socklen_t elen = sizeof(err);
                if (getsockopt(base_net_obj::_fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) {
                    err = errno;
                }
                if (err == 0) {
                    PDEBUG("connect ok %s:%d", _ip.c_str(), _port);
                    _status = CONNECT_OK;
                    connect_ok_process();
                } else {
                    THROW_COMMON_EXCEPT(std::string("connect failed after epoll: ") + strError(err));
                }
            }
            else 
            {
                if ((event & EPOLLIN) == EPOLLIN) //读
                {
                    base_connect<PROCESS>::real_recv();
                }

                if ((event & EPOLLOUT) == EPOLLOUT ) //写
                {
                    base_connect<PROCESS>::real_send();
                }	
            }
        }

        virtual void connect_ok_process()
        {
            PDEBUG("CONNECT OK");
            // 连接成功时，允许子类或已安装的 PROCESS 在此阶段安装编解码器等
            return;
        }

    protected:
        std::string _ip;
        unsigned short _port;
        CONNECT_STATUS _status;
};


#endif
