#include "../include/server.h"
#include "../include/multi_protocol_factory.h"
#include "../include/app_handler.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <signal.h>

class SimpleHttpsHandler : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        std::cout << "[HTTPS] " << req.method << " " << req.url << std::endl;
        
        res.status = 200;
        res.set_header("Content-Type", "text/plain");
        res.set_header("Server", "MyFrame-HTTPS/1.0");
        
        if (req.url == "/hello") {
            res.body = "Hello from HTTPS Server!\nFramework: MyFrame\nProtocol: HTTPS\nPort: 7777";
        } 
        else if (req.url == "/api/status") {
            res.set_header("Content-Type", "application/json");
            res.body = "{\"status\":\"running\",\"protocol\":\"https\",\"framework\":\"myframe\",\"port\":7777}";
        }
        else if (req.url == "/") {
            res.set_header("Content-Type", "text/html");
            res.body = "<html><body><h1>HTTPS Test Server</h1><p>Try <a href='/hello'>/hello</a></p></body></html>";
        }
        else {
            res.body = "HTTPS Server OK - Try /hello or /api/status";
        }
    }
    
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        send = WsFrame::text("Error: This is HTTPS-only server");
    }
    
    void on_connect() override {
        std::cout << "[HTTPS] 新连接" << std::endl;
    }
    
    void on_disconnect() override {
        std::cout << "[HTTPS] 连接断开" << std::endl;
    }
};

server* g_server = nullptr;

void signal_handler(int sig) {
    std::cout << "\n停止HTTPS服务器..." << std::endl;
    if (g_server) g_server->stop();
    exit(0);
}

int main(int argc, char** argv) {
    unsigned short port = 7777;
    if (argc > 1) port = (unsigned short)atoi(argv[1]);

    std::cout << "=== HTTPS专用服务器 ===" << std::endl;
    std::cout << "端口: " << port << std::endl;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server srv(1);
    g_server = &srv;
    
    auto handler = std::shared_ptr<SimpleHttpsHandler>(new SimpleHttpsHandler());
    auto factory = std::shared_ptr<MultiProtocolFactory>(new MultiProtocolFactory(handler.get(), MultiProtocolFactory::Mode::TlsOnly));
    
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();

    std::cout << "\n🔒 HTTPS服务器启动成功!" << std::endl;
    std::cout << "\n测试命令:" << std::endl;
    std::cout << "  curl -k https://127.0.0.1:" << port << "/hello" << std::endl;
    std::cout << "  curl -k https://127.0.0.1:" << port << "/api/status" << std::endl;
    std::cout << "  ./test_https_standalone " << port << std::endl;
    std::cout << "\n按 Ctrl+C 停止\n" << std::endl;

    while (true) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
}
