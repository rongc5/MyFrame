// HTTPS HEAD regression. TLS terminates inside MyFrame, then the same Level-1
// HTTP adapter must preserve representation length and suppress entity bytes.
#include "server.h"
#include "ssl_context.h"
#include "unified_protocol_factory.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

class HttpsHeadHandler : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest& req, myframe::HttpResponse& res) override {
        res.set_content_type("text/plain");
        if (req.url == "/legacy") {
            if (req.method != "HEAD") res.body = "abcdef";
        } else {
            res.body = "abcdef";
        }
    }
};

static int tcp_connect(unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr; std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); return -1;
    }
    return fd;
}

static std::string request(unsigned short port, const char* method, const char* path) {
    int fd = tcp_connect(port);
    if (fd < 0) return std::string();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    std::string response;
    if (SSL_connect(ssl) == 1) {
        std::string wire = std::string(method) + " " + path +
            " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
        size_t off = 0;
        while (off < wire.size()) {
            int n = SSL_write(ssl, wire.data() + off, static_cast<int>(wire.size() - off));
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        char buf[4096]; int n;
        while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) response.append(buf, static_cast<size_t>(n));
    }
    SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd);
    return response;
}

static std::string body_of(const std::string& response) {
    size_t pos = response.find("\r\n\r\n");
    return pos == std::string::npos ? std::string() : response.substr(pos + 4);
}

int main() {
    ssl_config conf;
    conf._cert_file = MYFRAME_TEST_CERT;
    conf._key_file = MYFRAME_TEST_KEY;
    conf._protocols = "TLSv1.2,TLSv1.3";
    conf._alpn = "http/1.1";
    tls_set_server_config(conf);

    const unsigned short port = 18080;
    HttpsHeadHandler handler;
    std::shared_ptr<myframe::UnifiedProtocolFactory> factory(new myframe::UnifiedProtocolFactory());
    factory->register_http_handler(&handler);
    server srv(2);
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

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

    srv.stop();
    if (g_fail == 0) { std::printf("test_https_head: ALL PASS\n"); return 0; }
    std::printf("test_https_head: %d FAIL\n", g_fail);
    return 1;
}
