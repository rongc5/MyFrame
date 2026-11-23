// Minimal blocking WebSocket client used by the P0 tests (server frames are
// unmasked; this client never sends data frames).
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace wstest {

inline unsigned short pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    unsigned short port = 0;
    if (fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) port = ntohs(addr.sin_port);
    }
    if (fd >= 0) ::close(fd);
    return port;
}

class Client {
public:
    Client() = default;
    ~Client() { close_fd(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // rcvbuf > 0 shrinks SO_RCVBUF before connecting (slow-consumer tests).
    bool connect_ws(unsigned short port, const std::string& user, int rcvbuf = 0, int timeout_ms = 3000) {
        close_fd();
        _buf.clear();
        _fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (_fd < 0) return false;
        if (rcvbuf > 0) ::setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        set_timeout(timeout_ms);
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { close_fd(); return false; }
        std::string req =
            "GET /ws HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Cookie: username=" + user + "\r\n\r\n";
        if (!send_all(req)) { close_fd(); return false; }
        // Read the 101 response; anything after the header is frame data.
        while (_buf.find("\r\n\r\n") == std::string::npos) {
            if (!fill()) { close_fd(); return false; }
            if (_buf.size() > 16384) { close_fd(); return false; }
        }
        size_t end = _buf.find("\r\n\r\n");
        bool ok = _buf.compare(0, 12, "HTTP/1.1 101") == 0;
        _buf.erase(0, end + 4);
        if (!ok) close_fd();
        return ok;
    }

    // 1 = frame read, 0 = timeout, -1 = closed/error.
    int read_frame(std::string& payload, int& opcode) {
        for (;;) {
            if (_buf.size() >= 2) {
                const unsigned char b0 = static_cast<unsigned char>(_buf[0]);
                const unsigned char b1 = static_cast<unsigned char>(_buf[1]);
                uint64_t len = b1 & 0x7f;
                size_t hdr = 2;
                if (len == 126) {
                    if (_buf.size() < 4) goto need_more;
                    len = (static_cast<unsigned char>(_buf[2]) << 8) | static_cast<unsigned char>(_buf[3]);
                    hdr = 4;
                } else if (len == 127) {
                    if (_buf.size() < 10) goto need_more;
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<unsigned char>(_buf[2 + i]);
                    hdr = 10;
                }
                if ((b1 & 0x80) != 0) return -1;  // server frames must not be masked
                if (_buf.size() >= hdr + len) {
                    opcode = b0 & 0x0f;
                    payload.assign(_buf, hdr, static_cast<size_t>(len));
                    _buf.erase(0, hdr + static_cast<size_t>(len));
                    return 1;
                }
            }
        need_more:
            int r = fill_once();
            if (r <= 0) return r;
        }
    }

    // Close with RST (SO_LINGER 0) to maximize server-side teardown races.
    void abort_close() {
        if (_fd >= 0) {
            linger lg; lg.l_onoff = 1; lg.l_linger = 0;
            ::setsockopt(_fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        }
        close_fd();
    }

    void close_fd() {
        if (_fd >= 0) { ::close(_fd); _fd = -1; }
    }

    int fd() const { return _fd; }
    void set_timeout(int ms) {
        if (_fd < 0) return;
        timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

private:
    bool send_all(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = ::send(_fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }
    bool fill() { return fill_once() > 0; }
    int fill_once() {
        char tmp[65536];
        ssize_t n = ::recv(_fd, tmp, sizeof(tmp), 0);
        if (n > 0) { _buf.append(tmp, static_cast<size_t>(n)); return 1; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && errno == EINTR) return fill_once();
        return -1;
    }

    int _fd{-1};
    std::string _buf;
};

} // namespace wstest
