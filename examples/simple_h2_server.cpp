#include "../include/server.h"
#include "../include/multi_protocol_factory.h"
#include "../include/app_handler.h"
#include "../core/ssl_context.h"
#include "../core/listen_factory.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <signal.h>
#include <cstdlib>

// Simple HTTP/2-over-TLS server example
// Notes:
// - Uses TLS with ALPN; if client offers "h2" it negotiates HTTP/2
// - Falls back to HTTP/1.1 if client does not support h2 (can be forced via env)
// - Business logic is shared via IAppHandler::on_http

class SimpleH2Handler : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        std::cout << "[H2] " << req.method << " " << req.url << std::endl;

        res.status = 200;
        res.set_header("Server", "MyFrame-H2/1.0");

        if (req.url == "/" || req.url == "/index.html") {
            res.set_header("Content-Type", "text/html; charset=utf-8");
            res.body = "<!doctype html><html><head><title>MyFrame H2 Server</title></head><body>"
                      "<h1>MyFrame HTTP/2 Server</h1>"
                      "<ul>"
                      "<li><a href='/hello'>/hello</a></li>"
                      "<li><a href='/api/status'>/api/status</a></li>"
                      "<li><a href='/big'>/big</a> (large body test)</li>"
                      "</ul>"
                      "</body></html>";
            return;
        }

        if (req.url == "/hello") {
            res.set_header("Content-Type", "text/plain");
            res.body = "Hello from HTTP/2 server!\nFramework: MyFrame\n";
            return;
        }

        if (req.url == "/api/status") {
            res.set_header("Content-Type", "application/json");
            res.body = "{\"status\":\"running\",\"protocol\":\"h2\",\"framework\":\"myframe\"}";
            return;
        }

        if (req.url == "/big") {
            // Large response body to exercise HTTP/2 DATA fragmentation and flow control.
            // Size can be overridden by env MYFRAME_BIG_SIZE (bytes)
            size_t sz = 2 * 1024 * 1024; // 2MB default
            const char* env = ::getenv("MYFRAME_BIG_SIZE");
            if (env && *env) {
                size_t v = strtoull(env, nullptr, 10);
                if (v > 0) sz = v;
            }
            res.set_header("Content-Type", "application/octet-stream");
            res.body.assign(sz, 'B');
            return;
        }

        res.set_header("Content-Type", "text/plain; charset=utf-8");
        res.body = "H2 server OK. Try /hello or /api/status";
    }

    void on_ws(const WsFrame& recv, WsFrame& send) override {
        (void)recv; // HTTP/2 example; no WS here
        send = WsFrame::text("Error: H2 server does not accept WebSocket on this port");
    }
};

static server* g_server = nullptr;

static void signal_handler(int) {
    std::cout << "\nStopping H2 server..." << std::endl;
    if (g_server) g_server->stop();
    std::exit(0);
}

int main(int argc, char** argv) {
    unsigned short port = 7779;
    if (argc > 1) port = (unsigned short)std::atoi(argv[1]);

    std::cout << "=== MyFrame HTTP/2 (TLS) Server ===" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "ALPN: offers h2 (falls back to http/1.1 if needed)" << std::endl;

    // Optional: force ALPN to h2 only by exporting before run:
    //   export MYFRAME_SSL_ALPN=h2

    ::signal(SIGINT,  signal_handler);
    ::signal(SIGTERM, signal_handler);

    // TLS certificate configuration (use project test certs)
    ssl_config conf;
    conf._cert_file = "/home/rong/myframe/test_certs/server.crt";
    conf._key_file  = "/home/rong/myframe/test_certs/server.key";
    conf._protocols = "TLSv1.2,TLSv1.3";
    conf._verify_peer = false;
    tls_set_server_config(conf);

    server srv(2); // 1 listen + 1 worker thread
    g_server = &srv;

    auto handler = std::make_shared<SimpleH2Handler>();
    auto biz = std::make_shared<MultiProtocolFactory>(handler.get(), MultiProtocolFactory::Mode::TlsOnly);
    auto lsn = std::make_shared<ListenFactory>("127.0.0.1", port, biz);
    srv.set_business_factory(lsn);
    try {
        srv.start();
    } catch (const std::exception& e) {
        std::cerr << "[fatal] failed to start: " << e.what() << std::endl; return 2;
    }

    std::cout << "\nHTTP/2 server started." << std::endl;
    std::cout << "Test with:" << std::endl;
    std::cout << "  curl -k --http2 https://127.0.0.1:" << port << "/hello" << std::endl;
    std::cout << "  curl -k --http2 https://127.0.0.1:" << port << "/big" << std::endl;
    std::cout << "  ./h2_client h2://127.0.0.1:" << port << "/hello" << std::endl;
    std::cout << "Press Ctrl+C to stop\n" << std::endl;

    while (true) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
}

