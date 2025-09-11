#ifndef __BASE_CONNECT_H__
#define __BASE_CONNECT_H__

#include "common_def.h"
#include "base_net_obj.h"
#include "common_obj_container.h"

#include "common_exception.h"
#include "common_epoll.h"
#include "codec.h"
#include "protocol_detect_process.h"
#include <memory>

template<class PROCESS>
class base_connect:public base_net_obj
{
    public:

        base_connect(const int32_t sock)
        {
            _fd = sock;
            int bReuseAddr = 1;
            setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &bReuseAddr, sizeof(bReuseAddr));
            _p_send_buf.reset();
            _process.reset();
            get_peer_addr();
        }

        base_connect()
        {
            _p_send_buf.reset();
            _process.reset();
        }

        virtual ~base_connect()
        {
            // unique_ptr members auto-clean
        }

        virtual void event_process(int event)
        {
            if ((event & EPOLLERR) == EPOLLERR || (event & EPOLLHUP) == EPOLLHUP)
            {
                THROW_COMMON_EXCEPT("epoll error "<< strError(errno).c_str());
            }

            if ((event & EPOLLIN) == EPOLLIN) //读
            {
                real_recv();
            }

            if ((event & EPOLLOUT) == EPOLLOUT ) //写
            {
                if (_codec) { _codec->on_writable_event(); }
                real_send();
            }	
        }
        virtual int real_net_process()
        {
            int32_t ret = 0;

            if ((get_event() & EPOLLIN) == EPOLLIN) {
                PDEBUG("real_net_process real_recv");
                real_recv(true);
            }

            if ((get_event() & EPOLLOUT) == EPOLLOUT) {
                PDEBUG("real_net_process real_send");
                real_send();
            }

            return ret;
        }

        virtual void destroy()
        {
            if (_process)
            {
                _process->destroy();
            }
        }

        PROCESS * process()
        {
            return _process.get();
        }

        void set_process(PROCESS *p)
        {
            // wrap raw pointer for backward compatibility
            _process.reset(p);
        }

        void set_process(std::unique_ptr<PROCESS> p)
        {
            _process = std::move(p);
        }

        virtual void notice_send()
        {
            update_event(_epoll_event | EPOLLOUT);

            if (_process)
            {
                real_send();
            }
        }

        virtual void handle_timeout(std::shared_ptr<timer_msg> & t_msg)
        {
            if (t_msg->_timer_type == DELAY_CLOSE_TIMER_TYPE) 
            {
                THROW_COMMON_EXCEPT("the connect obj delay close, delete it");
            }
            else if (t_msg->_timer_type == NONE_DATA_TIMER_TYPE) 
            {
                THROW_COMMON_EXCEPT("the connect obj no data");
            }
            
            _process->handle_timeout(t_msg);
        }

        virtual void handle_msg(std::shared_ptr<normal_msg> & p_msg)
        {
            _process->handle_msg(p_msg);
        }

    protected:
        virtual int RECV(void *buf, size_t len)
        {
            if (_codec) {
                int ret = (int)_codec->recv(_fd, (char*)buf, len);
                if (ret == 0)
                {
                    _process->peer_close();
                    THROW_COMMON_EXCEPT("the client close the socket(" << _fd << ")");
                }
                else if (ret < 0)
                {
                    if (errno != EAGAIN)
                    {
                        THROW_COMMON_EXCEPT("this socket occur fatal error " << strError(errno).c_str());
                    }
                    ret = 0;
                }
                return ret;
            }
            // During protocol detection, prefer MSG_PEEK so kernel buffer is untouched
            int flags = MSG_DONTWAIT;
            if (!_codec) {
                // dynamic cast is safe; base_data_process is polymorphic
                if (auto pd = dynamic_cast<protocol_detect_process*>(_process.get())) {
                    if (pd->want_peek()) {
                        flags |= MSG_PEEK;
                    }
                }
            }
            int ret = recv(_fd, buf, len, flags);
            if (ret == 0)
            {
                _process->peer_close();
                THROW_COMMON_EXCEPT("the client close the socket(" << _fd << ")");
            }
            else if (ret < 0)
            {
                if (errno != EAGAIN)
                {
                    THROW_COMMON_EXCEPT("this socket occur fatal error " << strError(errno).c_str());
                }
                ret = 0;
            }

            //PDEBUG("recv:%s", buf);

            return ret;
        }

        virtual ssize_t SEND(const void *buf, const size_t len)
        {
            if (len == 0)
            {
                return 0;
            }

            if (_codec) {
                ssize_t ret = _codec->send(_fd, (const char*)buf, len);
                if (ret < 0)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        THROW_COMMON_EXCEPT("send data error " << strError(errno).c_str());
                    }
                    ret = 0;
                }
                return ret;
            } else {
                ssize_t ret =  send(_fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (ret < 0)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        THROW_COMMON_EXCEPT("send data error " << strError(errno).c_str());
                    }
                    ret = 0;
                }
                return ret;
            }

        }

        void real_recv(int flag = false)
        {
            size_t _recv_buf_len = _recv_buf.length();
            size_t tmp_len = MAX_RECV_SIZE - _recv_buf_len; 	
            ssize_t ret = 0;
            size_t p_ret = 0;
            if (tmp_len > 0) //接收缓冲满了也可以先不接收
            {
                char t_buf[SIZE_LEN_32768];
                int r_len = tmp_len <= sizeof(t_buf) ? (int)tmp_len:(int)sizeof(t_buf);

                // During protocol detection (pre-TLS), peek bytes so we don't consume
                // the ClientHello before SSL_accept can read it. Once over TLS, do not peek.
                bool did_peek = false;
                if (!_codec && _process) {
                    // Only dynamic_cast when needed to avoid RTTI cost in hot path
                    auto detect = dynamic_cast<protocol_detect_process*>(_process.get());
                    if (detect && detect->want_peek()) {
                        ssize_t pr = ::recv(_fd, t_buf, r_len, MSG_DONTWAIT | MSG_PEEK);
                        if (pr > 0) {
                            ret = pr;
                            did_peek = true;
                        } else {
                            ret = pr; // pass through errors/EAGAIN handling below
                        }
                    }
                }

                if (!did_peek) {
                    ret = RECV(t_buf, r_len);
                }

                if (ret > 0){
                    _recv_buf.append(t_buf, ret);
                    _recv_buf_len += ret;
                }
            }

            if (_recv_buf_len > 0 || flag)
            {
                PDEBUG("process_recv_buf _recv_buf_len[%d] fd[%d], flag[%d]", _recv_buf_len, _fd, flag);
                p_ret = _process->process_recv_buf(_recv_buf.data(), _recv_buf_len);
                if (p_ret && p_ret <= _recv_buf_len)
                {
                    _recv_buf.erase(0, p_ret); 
                }
                else if (p_ret > _recv_buf_len)
                {
                    _recv_buf.erase(0, _recv_buf_len); 
                }
            }		

            PDEBUG("process_recv_buf _recv_buf[%d] ip[%s] flag[%d]", _recv_buf.length(), _peer_net.ip.c_str(), flag);
        }

        void real_send()
        {
            int i = 0;
            while (1)
            {
                if (i >= MAX_SEND_NUM)
                    break;

                if (!_p_send_buf)
                {
                    std::string* next = _process->get_send_buf();
                    if (next) _p_send_buf.reset(next);
                }

                if (!_p_send_buf) {
                    update_event(get_event() & ~EPOLLOUT);
                    break;
                }

                i++;

                size_t _send_buf_len = _p_send_buf->length();
                if (_p_send_buf && _send_buf_len)
                {
                    ssize_t ret = SEND(_p_send_buf->c_str(), _send_buf_len);				
                    if (ret == (ssize_t)_send_buf_len)
                    {
                        _p_send_buf.reset();
                    }
                    else if (ret > 0)
                    {
                        _p_send_buf->erase(0, ret);
                        PDEBUG("_p_send_buf erase %d", ret);
                    }
                }

                if (_p_send_buf && !_p_send_buf->length())
                {
                    _p_send_buf.reset();
                }
            }
        }

    protected:
        std::string _recv_buf;
        std::unique_ptr<std::string> _p_send_buf;
        std::unique_ptr<PROCESS> _process;
        std::unique_ptr<ICodec> _codec;

    public:
        void set_codec(std::unique_ptr<ICodec> codec) { _codec = std::move(codec); }
        ICodec* get_codec() const { return _codec.get(); }
};


#endif
