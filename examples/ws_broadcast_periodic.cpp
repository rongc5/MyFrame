#include "server.h"
#include "multi_protocol_factory.h"
#include "app_handler.h"
#include "../core/ws_push_hub.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>

class WsPeriodicApp : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        if (req.url == "/") {
            res.status = 200; res.set_content_type("text/html; charset=utf-8");
            res.body =
                "<html><body>"
                "<h3>WS Periodic Broadcast Demo</h3>"
                "<p>Connect via WebSocket: ws://127.0.0.1:7783/websocket</p>"
                "<p>Server will broadcast a JSON tick every 2s to all connections.</p>"
                "</body></html>";
            return;
        }
        res.status = 404; res.set_content_type("text/plain; charset=utf-8"); res.body = "Not Found";
    }
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        if (recv.opcode == WsFrame::PING || recv.payload == "ping") send = WsFrame::text("pong");
        else send = WsFrame::text("echo:" + recv.payload);
    }
};

int main(int argc, char** argv) {
    int port = 7783;
    if (argc > 1) port = std::atoi(argv[1]);
    WsPeriodicApp app;
    auto factory = std::make_shared<MultiProtocolFactory>(&app, MultiProtocolFactory::Mode::PlainOnly);
    server s(1);
    s.bind("0.0.0.0", (unsigned short)port);
    s.set_business_factory(factory);
    s.start();
    std::cout << "Periodic broadcast demo on http://127.0.0.1:" << port << "\n";

    std::atomic<bool> running{true};
    std::thread ticker([&](){
        int i = 0;
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::string payload = std::string("{\"type\":\"tick\",\"n\":") + std::to_string(++i) + "}";
            WsPushHub::Instance().BroadcastAll(payload);
        }
    });

    s.join();
    running.store(false);
    if (ticker.joinable()) ticker.join();
    return 0;
}
