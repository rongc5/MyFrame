#ifndef __CODEC_H__
#define __CODEC_H__

#include "common_def.h"
#include <sys/socket.h>
#include <errno.h>

struct ICodec {
    virtual ssize_t recv(int fd, char* buf, size_t len) = 0;
    virtual ssize_t send(int fd, const char* data, size_t len) = 0;
    // Optional hints/hooks for event-driven loops
    virtual int poll_events_hint() const { return 0; }
    virtual void on_writable_event() {}
    virtual ~ICodec() {}
};

class PlainCodec : public ICodec {
public:
    virtual ssize_t recv(int fd, char* buf, size_t len) override {
        ssize_t ret = ::recv(fd, buf, len, MSG_DONTWAIT);
        if (ret < 0 && errno != EAGAIN) {
            return ret; // real error
        } else if (ret < 0) {
            return 0; // EAGAIN
        }
        return ret;
    }

    virtual ssize_t send(int fd, const char* data, size_t len) override {
        ssize_t ret = ::send(fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return ret; // real error
        } else if (ret < 0) {
            return 0; // EAGAIN
        }
        return ret;
    }
};

#ifdef ENABLE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>

enum SSL_HANDSHAKE_STATUS
{
    SSL_HANDSHAKE_NONE = 0,
    SSL_HANDSHAKE_WANT_READ = 1,
    SSL_HANDSHAKE_WANT_WRITE = 2,
    SSL_HANDSHAKE_DONE = 3,
    SSL_HANDSHAKE_ERROR = 4
};

class SslCodec : public ICodec {
public:
    SslCodec(SSL* ssl) : _ssl(ssl), _handshake_done(false), _last_hs(SSL_HANDSHAKE_NONE) {}
    virtual ~SslCodec() {
        if (_ssl) {
            SSL_shutdown(_ssl);
            SSL_free(_ssl);
            _ssl = 0;
        }
    }

    bool is_handshake_done() const { return _handshake_done; }

    SSL_HANDSHAKE_STATUS ssl_handshake() {
        if (!_ssl) return SSL_HANDSHAKE_ERROR;
        if (_handshake_done) return SSL_HANDSHAKE_DONE;

        ERR_clear_error();
        int ret = SSL_accept(_ssl);
        if (ret == 1) {
            _handshake_done = true;
            _last_hs = SSL_HANDSHAKE_DONE;
            PDEBUG("[ssl] Handshake completed successfully");
            return SSL_HANDSHAKE_DONE;
        }

        int ssl_error = SSL_get_error(_ssl, ret);
        switch (ssl_error) {
            case SSL_ERROR_WANT_READ:
                _last_hs = SSL_HANDSHAKE_WANT_READ; return SSL_HANDSHAKE_WANT_READ;
            case SSL_ERROR_WANT_WRITE:
                _last_hs = SSL_HANDSHAKE_WANT_WRITE; return SSL_HANDSHAKE_WANT_WRITE;
            default:
                _last_hs = SSL_HANDSHAKE_ERROR; return SSL_HANDSHAKE_ERROR;
        }
    }

    virtual ssize_t recv(int fd, char* buf, size_t len) override {
        (void)fd; // SSL owns fd via SSL_set_fd
        if (!_ssl) return -1;
        if (!_handshake_done) {
            SSL_HANDSHAKE_STATUS hs = ssl_handshake();
            if (hs == SSL_HANDSHAKE_WANT_READ || hs == SSL_HANDSHAKE_WANT_WRITE) { errno = EAGAIN; return -1; }
            if (hs == SSL_HANDSHAKE_ERROR) { errno = EIO; return -1; }
        }
        int ret = SSL_read(_ssl, buf, (int)len);
        if (ret > 0) return ret;
        int ssl_error = SSL_get_error(_ssl, ret);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) { errno = EAGAIN; return -1; }
        if (ssl_error == SSL_ERROR_ZERO_RETURN) return 0; // clean close
        errno = EIO; return -1;
    }

    virtual ssize_t send(int fd, const char* data, size_t len) override {
        (void)fd;
        if (!_ssl) return -1;
        if (!_handshake_done) {
            SSL_HANDSHAKE_STATUS hs = ssl_handshake();
            if (hs == SSL_HANDSHAKE_WANT_READ || hs == SSL_HANDSHAKE_WANT_WRITE) { errno = EAGAIN; return -1; }
            if (hs == SSL_HANDSHAKE_ERROR) { errno = EIO; return -1; }
        }
        int ret = SSL_write(_ssl, data, (int)len);
        if (ret > 0) return ret;
        int ssl_error = SSL_get_error(_ssl, ret);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) { errno = EAGAIN; return -1; }
        errno = EIO; return -1;
    }

    virtual int poll_events_hint() const override {
        if (!_handshake_done && _last_hs == SSL_HANDSHAKE_WANT_WRITE) return EPOLLOUT;
        return 0;
    }

    virtual void on_writable_event() override {
        if (!_handshake_done && _last_hs == SSL_HANDSHAKE_WANT_WRITE) (void)ssl_handshake();
    }

private:
    SSL* _ssl;
    bool _handshake_done;
    SSL_HANDSHAKE_STATUS _last_hs;
};
#endif

#endif

