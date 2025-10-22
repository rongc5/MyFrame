// MyFrame Unified Protocol Architecture - Simple HTTP Example
// Demonstrates Level 1 API usage (IApplicationHandler)

#include "../include/server.h"
#include "../core/unified_protocol_factory.h"
#include "../core/app_handler_v2.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>

using namespace myframe;

// ============================================================================
// Simple HTTP Handler (Level 1)
// 只需实现 on_http()，框架处理所有协议细节
// ============================================================================

class SimpleHttpHandler : public IApplicationHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        std::cout << "[HTTP] " << req.method << " " << req.url << std::endl;

        if (req.url == "/") {
            res.set_html(
                "<html><body>"
                "<h1>MyFrame Unified Protocol Demo</h1>"
                "<p>Try: <a href='/api/hello'>/api/hello</a></p>"
                "<p>Try: <a href='/api/status'>/api/status</a></p>"
                "</body></html>"
            );
        }
        else if (req.url == "/api/hello") {
            res.set_json("{\"message\":\"Hello from MyFrame!\"}");
        }
        else if (req.url == "/api/status") {
            res.set_json("{\"status\":\"running\",\"framework\":\"myframe-v2\"}");
        }
        else {
            res.status = 404;
            res.set_text("404 Not Found");
        }
    }

    void on_connect(const ConnectionInfo& info) override {
        std::cout << "[CONNECT] " << info.remote_ip << ":" << info.remote_port << std::endl;
    }

    void on_disconnect() override {
        std::cout << "[DISCONNECT]" << std::endl;
    }
};

server* g_server = nullptr;

void signal_handler(int sig) {
    std::cout << "\nStopping server..." << std::endl;
    if (g_server) g_server->stop();
    exit(0);
}

int main(int argc, char** argv) {
    unsigned short port = 8080;
    if (argc > 1) port = (unsigned short)atoi(argv[1]);

    std::cout << "=== MyFrame Unified Protocol Demo ===" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "Architecture: Level 1 (IApplicationHandler)" << std::endl;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        // 创建服务器
        server srv(2);  // 1 listen + 1 worker
        g_server = &srv;

        // 创建业务处理器（Level 1）
        auto handler = std::make_shared<SimpleHttpHandler>();

        // 创建统一协议工厂
        auto factory = std::make_shared<UnifiedProtocolFactory>();

        // 注册 HTTP 处理器（Level 1）
        factory->register_http_handler(handler.get());

        // 配置服务器
        srv.bind("0.0.0.0", port);
        srv.set_business_factory(factory);

        // 启动
        srv.start();

        std::cout << "\n✅ Server started!" << std::endl;
        std::cout << "\nTest commands:" << std::endl;
        std::cout << "  curl http://127.0.0.1:" << port << "/" << std::endl;
        std::cout << "  curl http://127.0.0.1:" << port << "/api/hello" << std::endl;
        std::cout << "  curl http://127.0.0.1:" << port << "/api/status" << std::endl;
        std::cout << "\nPress Ctrl+C to stop\n" << std::endl;

        // 主循环
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
