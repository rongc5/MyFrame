#ifndef __LISTEN_CONNECT_H__
#define __LISTEN_CONNECT_H__

#include "common_def.h"

#include "common_exception.h"
#include "base_net_obj.h"
#include <memory>
#include <netinet/tcp.h>


template<class PROCESS>
class listen_connect:public base_net_obj
{
    public:
        listen_connect(const std::string &ip, unsigned short port)
        {
            _process.reset();

            _ip = ip;
            _port = port;

            struct sockaddr_in address; 
            int reuse_addr = 1;  

            memset((char *) &address, 0, sizeof(address)); 
            address.sin_family = AF_INET;
            address.sin_port = htons(port); 
            int ret = 0;         

            if (ip != "")        
            {
                inet_aton(ip.c_str(), &address.sin_addr);
            }
            else
            {
                address.sin_addr.s_addr = htonl(INADDR_ANY); 
            }

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)         
            {
                PDEBUG("[listen_factory] socket() failed: %s", strError(errno).c_str());
                THROW_COMMON_EXCEPT("socket error " << strError(errno).c_str());     
            }
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*)(&(reuse_addr)), sizeof(reuse_addr));
            int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void*)(&one), sizeof(one));

            if (::bind(fd, (struct sockaddr *) &address, sizeof(address)) < 0) 
            {
                PDEBUG("[listen_factory] bind(%s:%u) fail: %s", ip.empty()?"0.0.0.0":ip.c_str(), port, strError(errno).c_str());
                THROW_COMMON_EXCEPT("bind error "  << strError(errno).c_str() << " " << ip << ":" << port);
            }

            int backlog = 1024;
            if (const char* env_bl = ::getenv("MYFRAME_SOMAXCONN")) {
                int v = atoi(env_bl); if (v > 0) backlog = v;
            }
            ret = listen(fd, backlog);
            if (ret == -1)
            {
                PDEBUG("[listen_factory] listen(fd=%d, backlog=%d) fail: %s", fd, backlog, strError(errno).c_str());
                THROW_COMMON_EXCEPT("listen error "  << strError(errno).c_str());
            }

            set_unblock(fd);
            _fd  = fd;
            PDEBUG("[listen] bound %s:%u, fd=%d", ip.empty()?"0.0.0.0":ip.c_str(), port, _fd);
        }

        virtual ~listen_connect() = default;

        virtual void event_process(int events)
        {
            if ((events & EPOLLIN) == EPOLLIN)
            {
                PDEBUG("%s", "[accept] epoll in");
                int tmp_sock = 0;
                sockaddr_in addr;
                // 控制每次最多 accept N 次，避免长循环饿死其他事件
                constexpr int kMaxAcceptOnce = 128;
                int accepted = 0;
                while (accepted < kMaxAcceptOnce)
                {
                    socklen_t len = sizeof(addr);
                    tmp_sock = accept(_fd, (sockaddr*)&addr, &len);
                    if (tmp_sock != -1)
                    {				
                        PDEBUG("[accept] new fd=%d", tmp_sock);
                        if (_process) _process->process(tmp_sock);
                        ++accepted;
                    }
                    else
                    {
                        int err = errno;
                        if (err == EAGAIN || err == EWOULDBLOCK) {
                            // 边沿触发/非阻塞模式下，返回再试
                            break;
                        }
                        PDEBUG("accept fail:%s", strError(err).c_str());
                        break;
                    }
                }
            }
            else
            {
            }
        }


        virtual int real_net_process()
        {
            return 0;
        }

        void set_process(PROCESS *p)
        {
            _process.reset(p);

        }


    private:
        std::string _ip;
        unsigned short _port;
        std::unique_ptr<PROCESS> _process;
};


#endif
