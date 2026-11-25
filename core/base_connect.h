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
#include <algorithm>
#include <memory>
#include <deque>
#include <mutex>
#include <sys/uio.h>
#include <fcntl.h>

template<class PROCESS>
class base_connect:public base_net_obj
{
    public:

        base_connect(const int32_t sock)
            : _peek_drain_backlog(0)
        {
            _fd = sock;
            // The event loop requires non-blocking sockets: accept() does not
            // inherit O_NONBLOCK from the listener, and a blocking writev() to a
            // stalled peer would freeze the whole network thread.
            if (_fd >= 0) {
                int fl = ::fcntl(_fd, F_GETFL, 0);
                if (fl >= 0 && (fl & O_NONBLOCK) == 0) {
                    ::fcntl(_fd, F_SETFL, fl | O_NONBLOCK);
                }
            }
            int bReuseAddr = 1;
            setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &bReuseAddr, sizeof(bReuseAddr));
            _p_send_buf.reset();
            _send_offset = 0;
            _process.reset();
            get_peer_addr();
        }

        base_connect()
            : _peek_drain_backlog(0)
        {
            _p_send_buf.reset();
            _send_offset = 0;
            _process.reset();
        }

        virtual ~base_connect()
        {
            // unique_ptr members auto-clean
        }

        virtual void event_process(int event)
        {
            if ((event & EPOLLERR) == EPOLLERR) {
                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);
                if (getsockopt(_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 ||
                    socket_error == 0) {
                    socket_error = errno;
                }
                if (_process) _process->notify_peer_close();
                THROW_COMMON_EXCEPT("epoll socket error " << strError(socket_error).c_str());
            }

            bool peer_closed = (event & EPOLLHUP) == EPOLLHUP;
#ifdef EPOLLRDHUP
            peer_closed = peer_closed || (event & EPOLLRDHUP) == EPOLLRDHUP;
#endif

            if ((event & EPOLLIN) == EPOLLIN) // readable
            {
                touch_active(GetMonotonicMilliSecond());
                real_recv();
            }

            if ((event & EPOLLOUT) == EPOLLOUT) // writable
            {
                if (_codec) { _codec->on_writable_event(); }
                touch_active(GetMonotonicMilliSecond());
                real_send();
            }

            // RDHUP may arrive together with EPOLLIN. Process the final readable
            // bytes first, then enter the normal teardown path without reporting a
            // transport error for an ordinary browser connection close.
            if (peer_closed) {
                if (_process) _process->notify_peer_close();
                THROW_COMMON_EXCEPT("peer closed");
            }
        }
        virtual int real_net_process()
        {
            int32_t ret = 0;
            if (_fd < 0) {
                return ret;
            }

            if ((get_event() & EPOLLIN) == EPOLLIN) {
                PDEBUG("real_net_process real_recv");
                // Only force process when codec explicitly needs it (e.g., TLS handshake write->read)
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
            if (!on_owner_thread()) {
                // Teardown touches epoll and the fd: run it on the owner thread
                // through the standard container path.
                post_close_to_owner("destroy requested from foreign thread");
                return;
            }
            if (_destroyed) {
                return;
            }
            _destroyed = true;
            if (_process)
            {
                _process->destroy();
            }
            // FIX(fd-leak): close the fd on teardown immediately. App/session-layer
            // shared_ptr refs can outlive this teardown and delay ~base_net_obj,
            // leaving the fd stuck in CLOSE_WAIT (fd exhaustion -> crash). Detach
            // from epoll + close now; ~base_net_obj's close becomes a no-op.
            if (_p_net_container && _fd >= 0) {
                if (auto* ep = _p_net_container->get_epoll()) {
                    try { ep->del_from_epoll(this); } catch (...) {}
                }
            }
            // Release the codec while this object still owns the descriptor.
            // SslCodec's destructor calls SSL_shutdown() on the fd bound with
            // SSL_set_fd(); running it after ::close() lets the stale SSL write a
            // close_notify into whatever connection the kernel hands that fd
            // number to next (observed as "unexpected message" handshake
            // failures on fresh TLS clients).
            {
                std::lock_guard<std::recursive_mutex> lock(_send_mutex);
                _codec.reset();
            }
            if (_fd >= 0) { ::close(_fd); _fd = kInvalidFd; }
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
            if (!on_owner_thread()) {
                // Socket writes and _epoll_event belong to the owner thread.
                post_notice_send_to_owner();
                return;
            }
            if (_fd < 0) {
                return;  // closed: never write to a stale descriptor
            }
            std::lock_guard<std::recursive_mutex> lock(_send_mutex);
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
                if (has_unsent_outbound()) {
                    const uint64_t now = GetMonotonicMilliSecond();
                    if (!_close_when_drained) {
                        _close_when_drained = true;
                        _close_drain_start_ms = now;
                    }
                    // Allow queued outbound bytes (e.g. large HTTP Connection: close responses
                    // on non-blocking sockets) to drain while write progress continues.
                    if (now >= _last_active_ms && (now - _last_active_ms) < kMaxCloseDrainIdleMs &&
                        now >= _close_drain_start_ms && (now - _close_drain_start_ms) < (kMaxCloseDrainIdleMs * 4)) {
                        std::shared_ptr<timer_msg> retry(new timer_msg);
                        retry->_obj_id = get_id()._id;
                        retry->_timer_type = DELAY_CLOSE_TIMER_TYPE;
                        retry->_time_length = 50;
                        add_timer(retry);
                        return;
                    }
                }
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

        virtual bool flush_outbound() override {
            if (_fd < 0 || !_process || (_epoll_event & EPOLLOUT) == 0) return false;
            real_send();
            return _fd >= 0 && (_epoll_event & EPOLLOUT) != 0;
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
            if (_fd < 0) {
                return;
            }
            if (_peek_drain_backlog > 0) {
                size_t flushed = drain_peek_bytes(_peek_drain_backlog);
                _peek_drain_backlog -= std::min(_peek_drain_backlog, flushed);
            }
            // If protocol doesn't want to receive, and there is no codec
            // that may still need read events (e.g., TLS handshake), skip.
            if (_process && !_process->want_recv() && !_codec) {
                // Protocol not ready to receive (e.g., HTTP client still sending).
                // FIX(fd-leak): under EPOLLET the readable edge fires once. If the
                // peer closed (FIN), returning here without reading means we never
                // see EOF and the fd leaks in CLOSE_WAIT. Peek for EOF and tear the
                // connection down if the peer has closed.
                {
                    char _eof_probe;
                    ssize_t _pr = ::recv(_fd, &_eof_probe, 1, MSG_DONTWAIT | MSG_PEEK);
                    if (_pr == 0) {
                        _process->notify_peer_close();
                        THROW_COMMON_EXCEPT("peer closed while idle (no want_recv), fd=" << _fd);
                    }
                }
                return;
            }
            size_t _recv_buf_len = _recv_buf.length();
            // Guard against size_t underflow when the receive buffer already
            // holds MAX_RECV_SIZE bytes or more (e.g. a large pipelined burst).
            size_t tmp_len = (_recv_buf_len < static_cast<size_t>(MAX_RECV_SIZE))
                                 ? (static_cast<size_t>(MAX_RECV_SIZE) - _recv_buf_len)
                                 : 0;
            ssize_t ret = 0;
            size_t p_ret = 0;
            if (tmp_len > 0) // skip receiving while the receive buffer is full
            {
                char t_buf[SIZE_LEN_32768];
                int r_len = tmp_len <= sizeof(t_buf) ? (int)tmp_len:(int)sizeof(t_buf);

                // During protocol detection (pre-TLS), peek bytes so we don't consume
                // the ClientHello before SSL_accept can read it. Once over TLS, do not peek.
                bool did_peek = false;
                size_t peek_len = 0;
                if (!_codec && _process && _process->want_peek()) {
                    // Peek mode must never fall back to a consuming read: if the
                    // peek sees EAGAIN and the ClientHello arrives right after, a
                    // plain recv() would swallow it and SSL_accept() would then
                    // wait forever for bytes that are already gone.
                    ssize_t pr = ::recv(_fd, t_buf, r_len, MSG_DONTWAIT | MSG_PEEK);
                    if (pr > 0) {
                        ret = pr;
                        did_peek = true;
                        peek_len = static_cast<size_t>(pr);
                    } else if (pr == 0) {
                        _process->notify_peer_close();
                        THROW_COMMON_EXCEPT("the client close the socket(" << _fd << ")");
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        ret = 0;
                    } else {
                        THROW_COMMON_EXCEPT("this socket occur fatal error " << strError(errno).c_str());
                    }
                } else {
                    ret = RECV(t_buf, r_len);
                }

                if (ret > 0){
                    _recv_buf.append(t_buf, ret);
                    _recv_buf_len += ret;
                    touch_active(GetMonotonicMilliSecond());
                    if (did_peek) {
                        _peek_drain_backlog += peek_len;
                    }
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
                if (p_ret > 0 && _peek_drain_backlog > 0) {
                    if (_codec) {
                        // A TLS probe installs SslCodec while handling bytes read
                        // with MSG_PEEK.  Those bytes must remain in the socket so
                        // SSL_accept() can consume the complete ClientHello.
                        _peek_drain_backlog = 0;
                    } else {
                        size_t need = std::min(_peek_drain_backlog, static_cast<size_t>(p_ret));
                        size_t flushed = drain_peek_bytes(need);
                        _peek_drain_backlog -= flushed;
                    }
                }
            }        

            PDEBUG("process_recv_buf _recv_buf[%zu] ip[%s] flag[%d]", _recv_buf.length(), _peer_net.ip.c_str(), flag);
        }

        void real_send()
        {
            // notice_send() may originate in an application worker while the
            // epoll thread is already draining this connection. Serialize the
            // whole frame-header/body state machine so two writers cannot
            // splice WebSocket frames or consume the same queued payload.
            std::lock_guard<std::recursive_mutex> lock(_send_mutex);
            if (_fd < 0 || !_process) {
                return;  // closed: never write to a stale descriptor
            }
            // If SSL or custom codec is installed, fall back to single-buffer SEND path
            if (_codec) {
                // Cap each SSL_write() call: SSL_write() takes an int length,
                // so multi-gigabyte buffers must never be passed in one call.
                const size_t kMaxTlsWriteChunk = 1024 * 1024;
                int i = 0;
                while (i < MAX_SEND_NUM) {
                    i++;
                    if (!_p_send_buf) {
                        std::string* next = _process->get_send_buf();
                        if (!next) break;
                        _p_send_buf.reset(next);
                        _send_offset = 0;
                    }
                    if (_send_offset >= _p_send_buf->size()) {
                        _p_send_buf.reset();
                        _send_offset = 0;
                        continue;
                    }
                    const size_t remaining = _p_send_buf->size() - _send_offset;
                    const size_t chunk = std::min(remaining, kMaxTlsWriteChunk);
                    ssize_t ret = SEND(_p_send_buf->data() + _send_offset, chunk);
                    if (ret <= 0) {
                        // EAGAIN / SSL_ERROR_WANT_WRITE / SSL_ERROR_WANT_READ:
                        // wait for the next EPOLLOUT instead of spinning.
                        break;
                    }
                    // Actively draining: keep the connection alive so large
                    // HTTPS Connection: close responses do not trip the
                    // close-drain idle limit.
                    touch_active(GetMonotonicMilliSecond());
                    _send_offset += static_cast<size_t>(ret);
                    if (_send_offset >= _p_send_buf->size()) {
                        _p_send_buf.reset();
                        _send_offset = 0;
                    }
                }
                if (!_p_send_buf) {
                    if (std::string* more = _process->get_send_buf()) {
                        _p_send_buf.reset(more);
                        _send_offset = 0;
                    }
                }
                if (!_p_send_buf) {
                    update_event(get_event() & ~EPOLLOUT);
                    if (_close_when_drained) {
                        THROW_COMMON_EXCEPT("the connect obj delay close after drain");
                    }
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
                    _send_offset = 0;
                }
            }
            if (!_p_send_buf && _pending_send.empty()) {
                update_event(get_event() & ~EPOLLOUT);
                if (_close_when_drained) {
                    THROW_COMMON_EXCEPT("the connect obj delay close after drain");
                }
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
            if (_p_send_buf && _send_offset < _p_send_buf->size()) {
                iov[iovcnt].iov_base = (void*)(_p_send_buf->data() + _send_offset);
                iov[iovcnt].iov_len  = _p_send_buf->size() - _send_offset;
                total += iov[iovcnt].iov_len; iovcnt++;
            }
            for (size_t i=0; i<_pending_send.size() && iovcnt < MAX_IOV; ++i) {
                auto& sp = _pending_send[i]; if (!sp || sp->empty()) continue;
                iov[iovcnt].iov_base = (void*)sp->data();
                iov[iovcnt].iov_len  = sp->size();
                total += iov[iovcnt].iov_len; iovcnt++;
                if (total >= MAX_BATCH) break;
            }
            if (iovcnt == 0) {
                // Only empty buffers were queued: drop them so the cursor state
                // stays consistent, then finish exactly like a drained queue.
                _p_send_buf.reset();
                _send_offset = 0;
                _pending_send.clear();
                update_event(get_event() & ~EPOLLOUT);
                if (_close_when_drained) {
                    THROW_COMMON_EXCEPT("the connect obj delay close after drain");
                }
                return;
            }

            ssize_t wr = ::writev(_fd, iov, iovcnt);
            if (wr < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // keep EPOLLOUT
                    return;
                }
                THROW_COMMON_EXCEPT("sendv error " << strError(errno).c_str());
            }
            size_t left = (size_t)wr;
            if (left > 0) touch_active(GetMonotonicMilliSecond());
            // Consume from _p_send_buf then pending. Partial writes only move
            // the _send_offset cursor; buffers are never erased from the front,
            // which would cost O(N^2) memmove work on multi-megabyte bodies.
            if (_p_send_buf) {
                const size_t remaining = _send_offset < _p_send_buf->size()
                                             ? _p_send_buf->size() - _send_offset
                                             : 0;
                if (left >= remaining) {
                    left -= remaining;
                    _p_send_buf.reset();
                    _send_offset = 0;
                } else {
                    _send_offset += left;
                    left = 0;
                }
            }
            // consume from pending deque
            while (left > 0 && !_pending_send.empty()) {
                auto& front = _pending_send.front();
                size_t blen = front ? front->size() : 0;
                if (left >= blen) {
                    left -= blen;
                    _pending_send.pop_front();
                } else {
                    // _p_send_buf is necessarily empty here: it was fully
                    // consumed above before any pending byte could be written.
                    _p_send_buf = std::move(front);
                    _send_offset = left;
                    _pending_send.pop_front();
                    left = 0;
                }
            }
            // Promote the next pending buffer so the cursor always tracks it.
            while (!_p_send_buf && !_pending_send.empty()) {
                auto front = std::move(_pending_send.front());
                _pending_send.pop_front();
                if (front && !front->empty()) {
                    _p_send_buf = std::move(front);
                    _send_offset = 0;
                }
            }
            // If nothing left in local buffers, probe process once more before clearing EPOLLOUT
            if (!_p_send_buf && _pending_send.empty()) {
                if (std::string* more = _process->get_send_buf()) {
                    _p_send_buf.reset(more);
                    _send_offset = 0;
                }
            }
            if (!_p_send_buf && _pending_send.empty()) {
                update_event(get_event() & ~EPOLLOUT);
                if (_close_when_drained) {
                    THROW_COMMON_EXCEPT("the connect obj delay close after drain");
                }
            }
        }

    protected:
        bool has_unsent_outbound() const {
            return (_p_send_buf && _send_offset < _p_send_buf->size()) ||
                   !_pending_send.empty() ||
                   ((_epoll_event & EPOLLOUT) == EPOLLOUT);
        }
        size_t drain_peek_bytes(size_t need) {
            if (need == 0) return 0;
            char drop_buf[SIZE_LEN_32768];
            size_t flushed = 0;
            while (flushed < need) {
                size_t chunk = std::min(sizeof(drop_buf), need - flushed);
                ssize_t rc = ::recv(_fd, drop_buf, chunk, MSG_DONTWAIT);
                if (rc > 0) {
                    flushed += static_cast<size_t>(rc);
                } else if (rc == 0) {
                    if (_process) _process->notify_peer_close();
                    break;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    THROW_COMMON_EXCEPT("drain peek buffer error " << strError(errno).c_str());
                }
            }
            return flushed;
        }
        std::string _recv_buf;
        using send_buf_ptr = myframe::pooled_string_ptr;
        send_buf_ptr _p_send_buf;
        // Byte offset of the first unsent byte inside _p_send_buf.
        size_t _send_offset{0};
        std::unique_ptr<PROCESS> _process;
        std::unique_ptr<ICodec> _codec;
        std::deque<send_buf_ptr> _pending_send;
        std::recursive_mutex _send_mutex;
        size_t _peek_drain_backlog;
        bool _destroyed{false};
        bool _close_when_drained{false};
        uint64_t _close_drain_start_ms{0};
        static constexpr uint64_t kMaxCloseDrainIdleMs = 15000;

    public:
        void set_codec(std::unique_ptr<ICodec> codec) { _codec = std::move(codec); }
        ICodec* get_codec() const { return _codec.get(); }
};


#endif
