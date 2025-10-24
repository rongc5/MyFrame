#include "include/server.h"
#include "core/app_handler_v2.h"
#include "include/multi_protocol_factory.h"
#include "core/ssl_context.h"
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>

class SingleThreadHandler : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest& req, myframe::HttpResponse& res) override {
        std::cout << "[SINGLE-HTTP] " << req.method << " " << req.url << std::endl;
        
        res.status = 200;
        res.set_header("Content-Type", "text/plain");
        res.set_header("Server", "MyFrame-Single/1.0");
        
        if (req.url == "/hello") {
            res.body = "🚀 SINGLE-THREAD SERVER!\nFramework: MyFrame\nMode: Single Thread\nThreads: 1";
        } 
        else if (req.url == "/big") {
            size_t sz = 1024 * 1024; // 1MB 默认
            const char* env = ::getenv("MYFRAME_BIG_SIZE");
            if (env && *env) {
                unsigned long long v = strtoull(env, nullptr, 10);
                if (v > 0) sz = (size_t)v;
            }
            res.set_header("Content-Type", "application/octet-stream");
            res.body.assign(sz, 'S'); // 'S' for Single thread
            std::cout << "[SINGLE-HTTP] 生成大响应: " << sz << " bytes" << std::endl;
        }
        else {
            res.body = "Single Thread Server OK";
        }
        
        std::cout << "[SINGLE-HTTP] 响应: " << res.status << " (" << res.body.size() << " bytes)" << std::endl;
    }
    
    void on_ws(const myframe::WsFrame& recv, myframe::WsFrame& send) override {
        (void)recv;
        send = myframe::WsFrame::text("Single Thread WebSocket");
    }
    
    void on_connect(const myframe::ConnectionInfo&) override {
        std::cout << "[SINGLE-HTTP] 新连接 (单线程模式)" << std::endl;
    }
    
    void on_disconnect() override {
        std::cout << "[SINGLE-HTTP] 连接断开 (单线程模式)" << std::endl;
    }
};

std::unique_ptr<server> g_server;

void signal_handler(int sig) {
    std::cout << "\n停止单线程服务器..." << std::endl;
    if (g_server) {
        g_server->stop();
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    int port = 8108;  // 默认端口
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    std::cout << "=== MyFrame 单线程服务器 ===" << std::endl;
    std::cout << "端口: " << port << std::endl;
    std::cout << "线程: 1 (单线程模式)" << std::endl;
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        // 配置TLS证书
        ssl_config conf;
        conf._cert_file = "/home/rong/myframe/test_certs/server.crt";
        conf._key_file = "/home/rong/myframe/test_certs/server.key";
        conf._protocols = "TLSv1.2:TLSv1.3";
        conf._verify_peer = false;
        tls_set_server_config(conf);
        
        // 创建业务处理器
        std::shared_ptr<SingleThreadHandler> handler(new SingleThreadHandler());
        
        // 创建单线程服务器 - 重要：使用 server(1)
        g_server.reset(new server(1));
        
        // 绑定监听
        g_server->bind("127.0.0.1", port);
        
        // 使用多协议工厂 - TLS Only
        std::shared_ptr<MultiProtocolFactory> factory(
            new MultiProtocolFactory(
                handler.get(),
                MultiProtocolFactory::Mode::TlsOnly
            )
        );
        g_server->set_business_factory(factory);
        
        // 启动服务器
        g_server->start();
        
        std::cout << "\n🚀 单线程服务器启动成功！" << std::endl;
        std::cout << "\n测试命令:" << std::endl;
        std::cout << "  curl -k --http2 https://127.0.0.1:" << port << "/hello" << std::endl;
        std::cout << "  curl -k --http1.1 https://127.0.0.1:" << port << "/hello" << std::endl;
        std::cout << "  curl -k --http1.1 https://127.0.0.1:" << port << "/big" << std::endl;
        std::cout << "\n按 Ctrl+C 停止\n" << std::endl;

        // 主循环
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
