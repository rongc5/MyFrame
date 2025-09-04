#include "../include/server.h"
#include "../include/multi_protocol_factory.h"
#include "../include/app_handler.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <signal.h>

class SimpleWssHandler : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        std::cout << "[WSS-HTTP] " << req.method << " " << req.url << std::endl;
        
        res.status = 200;
        res.set_header("Server", "MyFrame-WSS/1.0");
        
        if (req.url == "/") {
            res.set_header("Content-Type", "text/html");
            res.body = "<html><body><h1>WSS Test Server</h1><p>WebSocket endpoint: /websocket</p></body></html>";
        } else {
            res.set_header("Content-Type", "text/plain");
            res.body = "WSS Server - WebSocket endpoint: /websocket";
        }
    }
    
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        std::cout << "[WSS] 收到: " << recv.payload << std::endl;
        
        if (recv.payload == "ping") {
            send = WsFrame::text("pong");
        } else if (recv.payload == "status") {
            send = WsFrame::text("{\"status\":\"running\",\"protocol\":\"wss\",\"port\":7778}");
        } else {
            send = WsFrame::text("Echo: " + recv.payload);
        }
        
        std::cout << "[WSS] 发送: " << send.payload << std::endl;
    }
    
    void on_connect() override {
        std::cout << "[WSS] WebSocket连接建立" << std::endl;
    }
    
    void on_disconnect() override {
        std::cout << "[WSS] WebSocket连接断开" << std::endl;
    }
};

server* g_server = nullptr;

void signal_handler(int sig) {
    std::cout << "\n停止WSS服务器..." << std::endl;
    if (g_server) g_server->stop();
    exit(0);
}

int main(int argc, char** argv) {
    unsigned short port = 7778;
    if (argc > 1) port = (unsigned short)atoi(argv[1]);

    std::cout << "=== WSS专用服务器 ===" << std::endl;
    std::cout << "端口: " << port << std::endl;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server srv(1);
    g_server = &srv;
    
    auto handler = std::shared_ptr<SimpleWssHandler>(new SimpleWssHandler());
    auto factory = std::shared_ptr<MultiProtocolFactory>(new MultiProtocolFactory(handler.get()));
    
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();

    std::cout << "\n🔒 WSS服务器启动成功!" << std::endl;
    std::cout << "\n测试命令:" << std::endl;
    std::cout << "  websocat wss://127.0.0.1:" << port << "/websocket" << std::endl;
    std::cout << "  ./test_wss_standalone " << port << std::endl;
    std::cout << "\n发送消息: ping, status, echo hello" << std::endl;
    std::cout << "\n按 Ctrl+C 停止\n" << std::endl;

    while (true) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
}
