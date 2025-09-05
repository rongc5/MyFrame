#pragma once

#include "codec.h"
#ifdef ENABLE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>

class ClientSslCodec : public ICodec {
public:
    explicit ClientSslCodec(SSL* ssl) : _ssl(ssl), _handshake_done(false), _last_hs(SSL_HANDSHAKE_NONE) {
        if (_ssl) {
            SSL_set_connect_state(_ssl);
            SSL_set_mode(_ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        }
    }
    virtual ~ClientSslCodec() {
        if (_ssl) { SSL_shutdown(_ssl); SSL_free(_ssl); _ssl = 0; }
    }

    bool is_handshake_done() const { return _handshake_done; }

    SSL_HANDSHAKE_STATUS ssl_handshake() {
        if (!_ssl) return SSL_HANDSHAKE_ERROR;
        if (_handshake_done) return SSL_HANDSHAKE_DONE;
        ERR_clear_error();
        int ret = SSL_connect(_ssl);
        if (ret == 1) { _handshake_done = true; _last_hs = SSL_HANDSHAKE_DONE; return SSL_HANDSHAKE_DONE; }
        int ssl_error = SSL_get_error(_ssl, ret);
        switch (ssl_error) {
            case SSL_ERROR_WANT_READ:  _last_hs = SSL_HANDSHAKE_WANT_READ;  return SSL_HANDSHAKE_WANT_READ;
            case SSL_ERROR_WANT_WRITE: _last_hs = SSL_HANDSHAKE_WANT_WRITE; return SSL_HANDSHAKE_WANT_WRITE;
            default: _last_hs = SSL_HANDSHAKE_ERROR; return SSL_HANDSHAKE_ERROR;
        }
    }

    virtual ssize_t recv(int fd, char* buf, size_t len) override {
        (void)fd;
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
        if (ssl_error == SSL_ERROR_ZERO_RETURN) return 0;
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


