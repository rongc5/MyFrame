#ifndef __BASE_CONNECT_H__
#define __BASE_CONNECT_H__

#include "common_def.h"
#include "base_net_obj.h"
#include "common_obj_container.h"

#include "common_exception.h"
#include "common_epoll.h"
#include "codec.h"
#include "protocol_detect_process.h"
#include "string_pool.h"
#include <memory>
#include <deque>
#include <sys/uio.h>

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
            if ((event & EPOLLERR) == EPOLLERR || (event & EPOLLHUP) == EPOLLHUP
#ifdef EPOLLRDHUP
                || (event & EPOLLRDHUP) == EPOLLRDHUP
#endif
            )
            {
                THROW_COMMON_EXCEPT("epoll error "<< strError(errno).c_str());
            }

            if ((event & EPOLLIN) == EPOLLIN) //读
            {
                touch_active(GetMilliSecond());
                real_recv();
            }

            if ((event & EPOLLOUT) == EPOLLOUT ) //写
            {
                if (_codec) { _codec->on_writable_event(); }
                touch_active(GetMilliSecond());
                real_send();
            }	
        }
        virtual int real_net_process()
        {
            int32_t ret = 0;

            if ((get_event() & EPOLLIN) == EPOLLIN) {
                PDEBUG("real_net_process real_recv");
                // Only force process when codec explicitly needs it (e.g., TLS handshake write→read)
                bool force = (_codec && _codec->poll_events_hint() != 0);
                real_recv(force);
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

        virtual bool wants_tick() const override {
            if (_codec && _codec->poll_events_hint() != 0) return true;
            return (_epoll_event & EPOLLOUT) == EPOLLOUT;
        }

    protected:
        virtual int RECV(void *buf, size_t len)
        {
            if (_codec) {
                int ret = (int)_codec->recv(_fd, (char*)buf, len);
                if (ret == 0)
                {
                    _process->notify_peer_close();
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
                _process->notify_peer_close();
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
            // If protocol doesn't want to receive, and there is no codec
            // that may still need read events (e.g., TLS handshake), skip.
            if (_process && !_process->want_recv() && !_codec) {
                // Protocol not ready to receive (e.g., HTTP client still sending)
                return;
            }
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
                if (!_codec && _process && _process->want_peek()) {
                    ssize_t pr = ::recv(_fd, t_buf, r_len, MSG_DONTWAIT | MSG_PEEK);
                    if (pr > 0) {
                        ret = pr;
                        did_peek = true;
                    } else {
                        ret = pr; // pass through errors/EAGAIN handling below
                    }
                }

                if (!did_peek) {
                    ret = RECV(t_buf, r_len);
                }

                if (ret > 0){
                    _recv_buf.append(t_buf, ret);
                    _recv_buf_len += ret;
                    touch_active(GetMilliSecond());
                }
            }

            if (_recv_buf_len > 0 || flag)
            {
                PDEBUG("process_recv_buf _recv_buf_len[%zu] fd[%d], flag[%d]", _recv_buf_len, _fd, flag);
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

            PDEBUG("process_recv_buf _recv_buf[%zu] ip[%s] flag[%d]", _recv_buf.length(), _peer_net.ip.c_str(), flag);
        }

        void real_send()
        {
            // If SSL or custom codec is installed, fall back to single-buffer SEND path
            if (_codec) {
                int i = 0;
                while (1) {
                    if (i >= MAX_SEND_NUM) break;
                    if (!_p_send_buf) {
                        std::string* next = _process->get_send_buf();
                        if (next) _p_send_buf.reset(next);
                    }
                    if (!_p_send_buf) { update_event(get_event() & ~EPOLLOUT); break; }
                    i++;
                    size_t len = _p_send_buf->length();
                    if (len) {
                        ssize_t ret = SEND(_p_send_buf->c_str(), len);
                        if (ret == (ssize_t)len) { _p_send_buf.reset(); }
                        else if (ret > 0) { _p_send_buf->erase(0, ret); PDEBUG("_p_send_buf erase %zd", ret); }
                    }
                    if (_p_send_buf && !_p_send_buf->length()) { _p_send_buf.reset(); }
                }
                return;
            }

            // Aggregated writev path (plain TCP)
            const int MAX_IOV = 16;
            const size_t MAX_BATCH = 64 * 1024; // 64KB per batch
            // Ensure we have a current buffer
            if (!_p_send_buf) {
                if (auto* next = _process->get_send_buf()) {
                    _p_send_buf.reset(next);
                }
            }
            if (!_p_send_buf && _pending_send.empty()) {
                update_event(get_event() & ~EPOLLOUT);
                return;
            }

            // Try to top up pending from process
            while ((int)_pending_send.size() + (_p_send_buf ? 1 : 0) < MAX_IOV) {
                std::string* nxt = _process->get_send_buf();
                if (!nxt) break;
                _pending_send.emplace_back(myframe::make_pooled_string(nxt));
            }

            // Build iovec array
            struct iovec iov[MAX_IOV]; int iovcnt = 0; size_t total = 0;
            if (_p_send_buf && !_p_send_buf->empty()) {
                iov[iovcnt].iov_base = (void*)_p_send_buf->data();
                iov[iovcnt].iov_len  = _p_send_buf->size();
                total += iov[iovcnt].iov_len; iovcnt++;
            }
            for (size_t i=0; i<_pending_send.size() && iovcnt < MAX_IOV; ++i) {
                auto& sp = _pending_send[i]; if (!sp || sp->empty()) continue;
                iov[iovcnt].iov_base = (void*)sp->data();
                iov[iovcnt].iov_len  = sp->size();
                total += iov[iovcnt].iov_len; iovcnt++;
                if (total >= MAX_BATCH) break;
            }
            if (iovcnt == 0) { update_event(get_event() & ~EPOLLOUT); return; }

            ssize_t wr = ::writev(_fd, iov, iovcnt);
            if (wr < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // keep EPOLLOUT
                    return;
                }
                THROW_COMMON_EXCEPT("sendv error " << strError(errno).c_str());
            }
            size_t left = (size_t)wr;
            if (left > 0) touch_active(GetMilliSecond());
            // Consume from _p_send_buf then pending
            if (_p_send_buf && left > 0) {
                size_t blen = _p_send_buf->size();
                if (left >= blen) { left -= blen; _p_send_buf.reset(); }
                else { _p_send_buf->erase(0, left); left = 0; }
            }
            // consume from pending deque
            while (left > 0 && !_pending_send.empty()) {
                auto& front = _pending_send.front();
                size_t blen = front->size();
                if (left >= blen) { left -= blen; _pending_send.pop_front(); }
                else { front->erase(0, left); left = 0; }
            }
            // Move partially-consumed first pending to _p_send_buf if needed
            if (!_p_send_buf && !_pending_send.empty()) {
                if (!_pending_send.front()->empty()) {
                    _p_send_buf = std::move(_pending_send.front());
                }
                _pending_send.pop_front();
            }
            // If nothing left, clear EPOLLOUT
            if (!_p_send_buf && _pending_send.empty()) {
                update_event(get_event() & ~EPOLLOUT);
            }
        }

    protected:
        std::string _recv_buf;
        using send_buf_ptr = myframe::pooled_string_ptr;
        send_buf_ptr _p_send_buf;
        std::unique_ptr<PROCESS> _process;
        std::unique_ptr<ICodec> _codec;
        std::deque<send_buf_ptr> _pending_send;

    public:
        void set_codec(std::unique_ptr<ICodec> codec) { _codec = std::move(codec); }
        ICodec* get_codec() const { return _codec.get(); }
};


#endif
