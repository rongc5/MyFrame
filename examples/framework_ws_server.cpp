#include "../include/server.h"
#include "../core/unified_protocol_factory.h"
#include "../core/protocol_context.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

using namespace myframe;

namespace {

// Level 2 handler: use protocol contexts for HTTP and WebSocket
class FrameworkWsHandler : public IProtocolHandler {
public:
    FrameworkWsHandler()
        : connection_count_(0)
        , message_count_(0) {}

    // Level 2 HTTP entry
    void on_http_request(HttpContext& ctx) override {
        const HttpRequest& req = ctx.request();
        HttpResponse& res = ctx.response();
        const auto host = req.get_header("Host");
        const std::string ws_host = host.empty() ? "127.0.0.1:19090" : host;

        if (req.url == "/" || req.url == "/index.html") {
            res.set_html(build_index_page(ws_host));
            return;
        }

        if (req.url == "/healthz") {
            std::ostringstream oss;
            oss << "{\"status\":\"ok\",\"connections\":" << connection_count_.load()
                << ",\"messages\":" << message_count_.load() << "}";
            res.set_json(oss.str());
            return;
        }

        res.status = 404;
        res.set_text("Not Found");
    }

    // Level 2 WS entry
    void on_ws_frame(WsContext& ctx) override {
        const auto& recv = ctx.frame();
        const auto msg_id = ++message_count_;
        std::cout << "[WS] payload: " << recv.payload << " (#" << msg_id << ")" << std::endl;

        if (recv.payload == "ping") {
            ctx.send_text("pong");
            return;
        }

        std::ostringstream oss;
        oss << "ack[" << msg_id << "]: " << recv.payload;
        ctx.send_text(oss.str());
    }

    void on_connect(const ConnectionInfo& info) override {
        const auto count = ++connection_count_;
        std::cout << "[WS] connection from " << info.remote_ip << ':' << info.remote_port
                  << " (active=" << count << ")" << std::endl;
    }

    void on_disconnect() override {
        const auto count = --connection_count_;
        std::cout << "[WS] disconnect (active=" << count << ")" << std::endl;
    }

private:
    static std::string build_index_page(const std::string& host) {
        std::ostringstream html;
        html << "<!DOCTYPE html><html><head><meta charset='utf-8'/>"
             << "<title>MyFrame WS Smoke Test</title></head><body>"
             << "<h2>MyFrame WebSocket Smoke Test</h2>"
             << "<p>WebSocket endpoint: ws://" << host << "/ws</p>"
             << "<button onclick='connectWs()'>Connect</button>"
             << "<button onclick='sendMsg()'>Send</button>"
             << "<button onclick='closeWs()'>Close</button>"
             << "<pre id='log' style='background:#eee;padding:10px;height:200px;overflow:auto'></pre>"
             << "<script>"
             << "let ws=null;"
             << "function log(msg){const node=document.getElementById('log');"
             << "node.textContent+=msg+'\\n';node.scrollTop=node.scrollHeight;}"
             << "function connectWs(){if(ws){log('Already connected');return;}ws=new WebSocket('ws://' + location.host + '/ws');"
             << "ws.onopen=()=>log('connected');ws.onclose=()=>{log('closed');ws=null;};"
             << "ws.onerror=e=>log('error:'+e.message);ws.onmessage=e=>log('recv:'+e.data);}"
             << "function sendMsg(){if(!ws||ws.readyState!==1){log('not connected');return;}"
             << "const msg='browser-' + new Date().toISOString();ws.send(msg);log('sent:'+msg);}"
             << "function closeWs(){if(ws){ws.close();}}"
             << "</script></body></html>";
        return html.str();
    }

    std::atomic<uint64_t> connection_count_;
    std::atomic<uint64_t> message_count_;
};

server* g_server = nullptr;
std::atomic<bool> g_running{true};

void handle_signal(int) {
    g_running = false;
    if (g_server) {
        std::cout << "\n[Server] shutting down..." << std::endl;
        g_server->stop();
    }
}

} // namespace

int main(int argc, char** argv) {
    unsigned short port = 19090;
    if (argc > 1) {
        port = static_cast<unsigned short>(std::stoi(argv[1]));
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "=== MyFrame WS Framework Server ===" << std::endl;
    std::cout << "Listening on ws://0.0.0.0:" << port << "/ws" << std::endl;

    try {
        server srv(2);
        g_server = &srv;

        auto handler = std::make_shared<FrameworkWsHandler>();
        auto factory = std::make_shared<UnifiedProtocolFactory>();
        // Level 2 registration: use context handlers
        factory->register_http_context_handler(handler.get());
        factory->register_ws_context_handler(handler.get());

        srv.bind("0.0.0.0", port);
        srv.set_business_factory(factory);
        srv.start();

        std::cout << "[Server] ready. Try curl http://127.0.0.1:" << port << "/healthz" << std::endl;

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "[Server] fatal: " << ex.what() << std::endl;
        return 1;
    }
}
