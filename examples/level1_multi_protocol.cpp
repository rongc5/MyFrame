// MyFrame Level 1 Demo - HTTP/WS/HTTPS/WSS/HTTP2 on IApplicationHandler
// Demonstrates synchronous handlers plus new helper APIs (schedule_timeout,
// get_current_thread, current_connection_id).

#include "server.h"
#include "multi_protocol_factory.h"
#include "app_handler_v2.h"
#include "ssl_context.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace myframe;

namespace {

class Level1Handler : public IApplicationHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        auto conn_id = current_connection_id();
        std::cout << "[HTTP] " << req.method << " " << req.url
                  << " (conn=" << conn_id._id << ", thread=" << conn_id._thread_index << ")"
                  << std::endl;

        if (req.url == "/") {
            res.set_html(R"HTML(
<!DOCTYPE html>
<html><head><title>Level 1 Demo</title></head><body>
<h1>Level 1 (IApplicationHandler)</h1>
<ul>
  <li><a href="/api/hello">/api/hello</a></li>
  <li><a href="/api/slow">/api/slow</a> (demonstrates schedule_timeout)</li>
  <li><a href="/api/thread">/api/thread</a> (shows get_current_thread)</li>
</ul>
<p>WebSocket echo endpoint: <code>/ws</code></p>
</body></html>
)HTML");
        } else if (req.url == "/api/hello") {
            res.set_json("{\"message\":\"hello from level1\"}");
        } else if (req.url == "/api/thread") {
            auto* th = get_current_thread();
            std::stringstream json;
            json << "{\"thread_available\":"
                 << (th ? "true" : "false")
                 << ",\"connection\":" << conn_id._id << "}";
            res.set_json(json.str());
        } else if (req.url == "/api/slow") {
            uint32_t timer_id = schedule_timeout(500);
            std::stringstream json;
            json << "{\"timer_id\":" << timer_id
                 << ",\"note\":\"response sent immediately; check logs for timeout callback\"}";
            res.set_json(json.str());
        } else {
            res.status = 404;
            res.set_text("404 Not Found");
        }
    }

    void on_ws(const WsFrame& recv, WsFrame& send) override {
        auto conn_id = current_connection_id();
        std::cout << "[WS] opcode=" << static_cast<int>(recv.opcode)
                  << " payload='" << recv.payload << "'"
                  << " (conn=" << conn_id._id << ")" << std::endl;
        schedule_timeout(1000);
        if (recv.opcode == WsFrame::TEXT) {
            send = WsFrame::text("Echo: " + recv.payload);
        } else {
            send = WsFrame::text("Binary frame (echo suppressed)");
        }
    }

    void on_connect(const ConnectionInfo& info) override {
        std::cout << "[CONNECT] " << info.remote_ip << ":" << info.remote_port
                  << " -> conn_id=" << info.connection_id << std::endl;
    }

    void on_disconnect() override {
        std::cout << "[DISCONNECT]" << std::endl;
    }

    void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) override {
        if (!t_msg) return;
        std::cout << "[TIMEOUT] timer_id=" << t_msg->_timer_id
                  << " type=" << t_msg->_timer_type
                  << " (conn=" << t_msg->_obj_id << ")" << std::endl;
    }

    void handle_msg(std::shared_ptr<::normal_msg>& msg) override {
        if (!msg) return;
        std::cout << "[MSG] op=" << msg->_msg_op << std::endl;
    }
};

struct Options {
    unsigned short port = 8080;
    bool enable_tls = false;
    std::string cert;
    std::string key;
};

Options parse_args(int argc, char* argv[]) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            opt.port = static_cast<unsigned short>(std::atoi(argv[++i]));
        } else if (a == "--tls") {
            opt.enable_tls = true;
        } else if (a == "--cert" && i + 1 < argc) {
            opt.cert = argv[++i];
        } else if (a == "--key" && i + 1 < argc) {
            opt.key = argv[++i];
        }
    }
    return opt;
}

server* g_srv = nullptr;

void stop_server(int) {
    std::cout << "\nStopping Level 1 demo..." << std::endl;
    if (g_srv) g_srv->stop();
    std::exit(0);
}

} // namespace

int main(int argc, char* argv[]) {
    Options opt = parse_args(argc, argv);

    std::cout << "==============================" << std::endl;
    std::cout << " Level 1 Multi-Protocol Demo" << std::endl;
    std::cout << "==============================" << std::endl;
    std::cout << "Port        : " << opt.port << std::endl;
    std::cout << "TLS Enabled : " << (opt.enable_tls ? "yes" : "no (HTTP/WS)") << std::endl;
    std::cout << "Protocols   : HTTP" << (opt.enable_tls ? "/HTTPS" : "")
              << " + WS" << (opt.enable_tls ? "/WSS + HTTP/2" : "") << std::endl;

    if (opt.enable_tls) {
        ssl_config conf;
        conf._cert_file = !opt.cert.empty() ? opt.cert : "test_certs/server.crt";
        conf._key_file  = !opt.key.empty()  ? opt.key  : "test_certs/server.key";
        conf._protocols = "TLSv1.2,TLSv1.3";
        conf._enable_session_cache = true;
        tls_set_server_config(conf);
        std::cout << "TLS certificate: " << conf._cert_file << std::endl;
        std::cout << "TLS key       : " << conf._key_file << std::endl;
        std::cout << "ALPN          : default (h2,http/1.1)" << std::endl;
    }

    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);

    try {
        auto handler = std::make_shared<Level1Handler>();
        std::shared_ptr<MultiProtocolFactory> mpf(
            new MultiProtocolFactory(handler.get(),
                                      opt.enable_tls ? MultiProtocolFactory::Mode::Auto
                                                     : MultiProtocolFactory::Mode::Auto));

        server srv(3); // 1 listen + 2 worker threads for demo
        g_srv = &srv;
        srv.bind("0.0.0.0", opt.port);
        srv.set_business_factory(mpf);
        srv.start();

        std::cout << "\nTest commands:" << std::endl;
        std::cout << "  curl " << (opt.enable_tls ? "-k " : "")
                  << (opt.enable_tls ? "https" : "http")
                  << "://127.0.0.1:" << opt.port << "/api/hello" << std::endl;
        std::cout << "  websocat " << (opt.enable_tls ? "wss" : "ws")
                  << "://127.0.0.1:" << opt.port << "/ws" << std::endl;
        if (opt.enable_tls) {
            std::cout << "  nghttp -n " << opt.port << " https://127.0.0.1:" << opt.port << "/api/hello"
                      << "  # HTTP/2" << std::endl;
        }

        srv.join();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
