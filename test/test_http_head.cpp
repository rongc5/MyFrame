// Level-1 HTTP HEAD regression: representation length is preserved when known,
// no entity bytes are sent, and legacy empty-body handlers never emit CL: 0.
#include "server.h"
#include "unified_protocol_factory.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

class HeadHandler : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest& req, myframe::HttpResponse& res) override {
        res.set_content_type("text/plain");
        if (req.url == "/legacy") {
            // Pattern used by older applications: build no body for HEAD.
            if (req.method != "HEAD") res.body = "abcdef";
        } else if (req.url == "/explicit") {
            if (req.method == "HEAD") res.set_header("Content-Length", "6");
            else res.body = "abcdef";
        } else {
            // Preferred pattern: build the GET representation for HEAD too;
            // the framework suppresses it at the transport boundary.
            res.body = "abcdef";
        }
    }
};

static int dial(unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr; std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); return -1;
    }
    return fd;
}

static std::string request(unsigned short port, const char* method, const char* path) {
    int fd = dial(port);
    if (fd < 0) return std::string();
    std::string wire = std::string(method) + " " + path +
        " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
    size_t off = 0;
    while (off < wire.size()) {
        ssize_t n = ::send(fd, wire.data() + off, wire.size() - off, 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
    std::string response;
    char buf[4096]; ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) response.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return response;
}

static std::string body_of(const std::string& response) {
    size_t pos = response.find("\r\n\r\n");
    return pos == std::string::npos ? std::string() : response.substr(pos + 4);
}

int main() {
    const unsigned short port = 18079;
    HeadHandler handler;
    std::shared_ptr<myframe::UnifiedProtocolFactory> factory(new myframe::UnifiedProtocolFactory());
    factory->register_http_handler(&handler);
    server srv(2);
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();
    for (int i = 0; i < 100; ++i) {
        int fd = dial(port);
        if (fd >= 0) { ::close(fd); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::string get = request(port, "GET", "/normal");
    CHECK(get.find("Content-Length: 6") != std::string::npos);
    CHECK(body_of(get) == "abcdef");

    std::string head = request(port, "HEAD", "/normal");
    CHECK(head.find("Content-Length: 6") != std::string::npos);
    CHECK(body_of(head).empty());

    std::string legacy = request(port, "HEAD", "/legacy");
    CHECK(legacy.find("Content-Length: 0") == std::string::npos);
    CHECK(legacy.find("Content-Length:") == std::string::npos);
    CHECK(body_of(legacy).empty());

    std::string explicit_head = request(port, "HEAD", "/explicit");
    CHECK(explicit_head.find("Content-Length: 6") != std::string::npos);
    CHECK(body_of(explicit_head).empty());

    srv.stop();
    if (g_fail == 0) { std::printf("test_http_head: ALL PASS\n"); return 0; }
    std::printf("test_http_head: %d FAIL\n", g_fail);
    return 1;
}
