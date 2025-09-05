#include "../include/server.h"
#include "../include/multi_protocol_factory.h"
#include "../include/app_handler.h"
#include "../core/listen_factory.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <signal.h>

class SimpleHttpHandler : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        std::cout << "[HTTP] " << req.method << " " << req.url << std::endl;
        
        res.status = 200;
        res.set_header("Content-Type", "text/plain");
        res.set_header("Server", "MyFrame-HTTP/1.0");
        
        if (req.url == "/hello") {
            res.body = "Hello from HTTP Server!\nFramework: MyFrame\nProtocol: HTTP\nPort: 8080";
        } 
        else if (req.url == "/api/status") {
            res.set_header("Content-Type", "application/json");
            res.body = "{\"status\":\"running\",\"protocol\":\"http\",\"framework\":\"myframe\",\"port\":8080}";
        }
        else if (req.url == "/") {
            res.set_header("Content-Type", "text/html");
            res.body = "<html><body><h1>HTTP Test Server</h1><p>Try <a href='/hello'>/hello</a></p></body></html>";
        }
        else {
            res.body = "HTTP Server OK - Try /hello or /api/status";
        }
    }
    
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        send = WsFrame::text("Error: This is HTTP-only server");
    }
    
    void on_connect() override {
        std::cout << "[HTTP] 新连接" << std::endl;
    }
    
    void on_disconnect() override {
        std::cout << "[HTTP] 连接断开" << std::endl;
    }
};

server* g_server = nullptr;

void signal_handler(int sig) {
    std::cout << "\n停止HTTP服务器..." << std::endl;
    if (g_server) g_server->stop();
    exit(0);
}

int main(int argc, char** argv) {
    unsigned short port = 8080;
    if (argc > 1) port = (unsigned short)atoi(argv[1]);

    std::cout << "=== HTTP服务器 ===" << std::endl;
    std::cout << "端口: " << port << std::endl;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server srv; // 默认2线程：1个listen + 1个worker
    g_server = &srv;
    
    auto handler = std::shared_ptr<SimpleHttpHandler>(new SimpleHttpHandler());
    auto biz = std::make_shared<MultiProtocolFactory>(handler.get());
    auto lsn = std::make_shared<ListenFactory>("127.0.0.1", port, biz);
    srv.set_business_factory(lsn);
    srv.start();

    std::cout << "\n📡 HTTP服务器启动成功!" << std::endl;
    std::cout << "\n测试命令:" << std::endl;
    std::cout << "  curl http://127.0.0.1:" << port << "/hello" << std::endl;
    std::cout << "  curl http://127.0.0.1:" << port << "/api/status" << std::endl;
    std::cout << "\n按 Ctrl+C 停止\n" << std::endl;

    while (true) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
}
