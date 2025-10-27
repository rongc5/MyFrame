// MyFrame Level 2 Demo - Unified Handler with async HTTP, WebSocket and custom binary
// Supports HTTP/HTTPS, WS/WSS and HTTP/2 when TLS is enabled.

#include "server.h"
#include "unified_protocol_factory.h"
#include "protocol_context.h"
#include "ssl_context.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

using namespace myframe;

namespace {

struct EchoBinaryMessage : public normal_msg {
    std::string payload;
    EchoBinaryMessage() : normal_msg(2001) {}
};

class Level2Handler : public IProtocolHandler {
public:
    void on_http_request(HttpContext& ctx) override {
        const auto& req = ctx.request();
        auto conn_id = current_connection_id();
        auto* th = get_current_thread();
        std::cout << "[L2 HTTP] " << req.method << " " << req.url
                  << " (conn=" << conn_id._id
                  << ", thread_available=" << (th ? "true" : "false")
                  << ")" << std::endl;

        if (req.url == "/") {
            ctx.response().set_html(R"HTML(
<!DOCTYPE html>
<html><head><title>Level 2 Demo</title></head><body>
<h1>Level 2 (IProtocolHandler)</h1>
<ul>
  <li><a href="/sync">/sync</a> - immediate JSON response</li>
  <li><a href="/async">/async</a> - background thread completes response</li>
  <li><a href="/timer">/timer</a> - schedules timeout callback</li>
</ul>
<p>WebSocket echo endpoint: <code>/ws</code></p>
<p>Binary (TLV) endpoint: send bytes <code>ECHO<length><payload></code></p>
</body></html>
)HTML");
            return;
        }

        if (req.url == "/sync") {
            ctx.response().set_json("{\"type\":\"sync\",\"status\":\"ok\"}");
            return;
        }

        if (req.url == "/timer") {
            uint32_t timer_id = schedule_timeout(750);
            ctx.keep_alive(true);
            ctx.response().set_json(std::string("{\"timer_id\":") + std::to_string(timer_id) + "}");
            return;
        }

        if (req.url == "/async") {
            // Keep a local copy of request id for logging
            auto req_id = ctx.connection_info().connection_id;
            ctx.async_response([&, req_id]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::stringstream json;
                json << "{\"type\":\"async\",\"request_id\":" << req_id << "}";

                auto payload = json.str();
                auto task = std::make_shared<HttpContextTaskMessage>(
                    [payload](HttpContext& context) {
                        context.response().set_json(payload);
                        context.complete_async_response();
                    });
                ctx.send_msg(task);
            });
            return;
        }

        ctx.response().status = 404;
        ctx.response().set_text("Not Found");
    }

    void on_ws_frame(WsContext& ctx) override {
        const auto& frame = ctx.frame();
        auto conn_id = current_connection_id();
        std::cout << "[L2 WS] opcode=" << static_cast<int>(frame.opcode)
                  << " payload='" << frame.payload << "'"
                  << " (conn=" << conn_id._id << ")" << std::endl;
        schedule_timeout(1500);
        if (frame.opcode == WsFrame::TEXT) {
            ctx.send_text("[ws] echo: " + frame.payload);
        } else if (frame.opcode == WsFrame::PING) {
            ctx.send_pong();
        }
    }

    void on_binary_message(BinaryContext& ctx) override {
        std::string payload(reinterpret_cast<const char*>(ctx.payload()), ctx.payload_size());
        std::cout << "[L2 BIN] proto_id=" << ctx.protocol_id()
                  << " payload='" << payload << "'" << std::endl;

        std::string reply = "ACK: " + payload;
        ctx.send(ctx.protocol_id(), reply.data(), reply.size());

        // Demonstrate cross-thread message by posting to handler
        auto msg = std::make_shared<EchoBinaryMessage>();
        msg->payload = payload;
        ctx.send_msg(msg);
    }

    bool handle_thread_msg(std::shared_ptr<::normal_msg>& msg) override {
        if (!msg) {
            return true;
        }
        if (msg->_msg_op == 2001) {
            auto casted = std::static_pointer_cast<EchoBinaryMessage>(msg);
            std::cout << "[L2 MSG] payload='" << casted->payload << "'" << std::endl;
        } else {
            std::cout << "[L2 MSG] op=" << msg->_msg_op << std::endl;
        }
        return true;
    }

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (!t_msg) return;
        std::cout << "[L2 TIMEOUT] timer_id=" << t_msg->_timer_id
                  << " type=" << t_msg->_timer_type
                  << " conn=" << t_msg->_obj_id << std::endl;
    }
};

struct Options {
    unsigned short port = 9090;
    bool enable_tls = false;
    std::string cert;
    std::string key;
};

Options parse_args(int argc, char* argv[]) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            opt.port = static_cast<unsigned short>(std::atoi(argv[++i]));
        } else if (arg == "--tls") {
            opt.enable_tls = true;
        } else if (arg == "--cert" && i + 1 < argc) {
            opt.cert = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            opt.key = argv[++i];
        }
    }
    return opt;
}

server* g_srv = nullptr;

void stop_server(int) {
    std::cout << "\nStopping Level 2 demo..." << std::endl;
    if (g_srv) g_srv->stop();
    std::exit(0);
}

} // namespace

int main(int argc, char* argv[]) {
    Options opt = parse_args(argc, argv);

    std::cout << "==============================" << std::endl;
    std::cout << " Level 2 Multi-Protocol Demo" << std::endl;
    std::cout << "==============================" << std::endl;
    std::cout << "Port        : " << opt.port << std::endl;
    std::cout << "TLS Enabled : " << (opt.enable_tls ? "yes" : "no") << std::endl;

    if (opt.enable_tls) {
        ssl_config conf;
        conf._cert_file = !opt.cert.empty() ? opt.cert : "test_certs/server.crt";
        conf._key_file  = !opt.key.empty()  ? opt.key  : "test_certs/server.key";
        conf._protocols = "TLSv1.2,TLSv1.3";
        conf._enable_session_cache = true;

        // Level 2 demo only implements HTTP/1.1; tell TLS layer to skip h2 ALPN.
        ::setenv("MYFRAME_SSL_ALPN", "http/1.1", 1);
        tls_set_server_config(conf);

        std::cout << "TLS certificate: " << conf._cert_file << std::endl;
        std::cout << "TLS key       : " << conf._key_file << std::endl;
        std::cout << "ALPN          : http/1.1 (HTTP/2 disabled)" << std::endl;
    }

    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);

    try {
        server srv(3);
        g_srv = &srv;

        auto handler = std::make_shared<Level2Handler>();
        auto factory = std::make_shared<UnifiedProtocolFactory>();

        factory->register_http_context_handler(handler.get());
        factory->register_ws_context_handler(handler.get());

        auto detect_binary = [](const char* data, size_t len) -> bool {
            return len >= 4 && std::memcmp(data, "ECHO", 4) == 0;
        };
        factory->register_binary_context_handler("echo", detect_binary, handler.get(), 40);

        srv.bind("0.0.0.0", opt.port);
        srv.set_business_factory(factory);
        srv.start();

        std::cout << "\nTest commands:" << std::endl;
        std::string scheme = opt.enable_tls ? "https" : "http";
        std::string wsscheme = opt.enable_tls ? "wss" : "ws";
        std::cout << "  curl " << (opt.enable_tls ? "-k " : "")
                  << scheme << "://127.0.0.1:" << opt.port << "/sync" << std::endl;
        std::cout << "  curl " << (opt.enable_tls ? "-k " : "")
                  << scheme << "://127.0.0.1:" << opt.port << "/async" << std::endl;
        std::cout << "  websocat " << wsscheme << "://127.0.0.1:" << opt.port << "/ws" << std::endl;
        std::cout << "  printf 'ECHO\\0\\0\\0\\x05hello' | nc 127.0.0.1 " << opt.port
                  << "  # binary TLV" << std::endl;
        if (opt.enable_tls) {
            std::cout << "  # HTTP/2 is disabled in this Level 2 demo (ALPN -> http/1.1)" << std::endl;
        }

        srv.join();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
