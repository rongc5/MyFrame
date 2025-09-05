#include "../include/server.h"
#include "../include/app_handler.h"
#include "../include/multi_protocol_factory.h"
#include "../core/ssl_context.h"
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>

class MultiProtocolHandler : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        std::cout << "[MULTI-HTTP] " << req.method << " " << req.url << std::endl;
        
        res.status = 200;
        res.set_header("Content-Type", "text/plain");
        res.set_header("Server", "MyFrame-Multi/1.0");
        
        if (req.url == "/hello") {
            res.body = "🚀 MULTI-PROTOCOL SERVER!\nFramework: MyFrame\nProtocols: HTTPS + WSS\nMode: Multi-Protocol Detection";
        } 
        else if (req.url == "/big") {
            size_t sz = 1024 * 1024; // 1MB 默认
            const char* env = ::getenv("MYFRAME_BIG_SIZE");
            if (env && *env) {
                unsigned long long v = strtoull(env, nullptr, 10);
                if (v > 0) sz = (size_t)v;
            }
            res.set_header("Content-Type", "application/octet-stream");
            res.body.assign(sz, 'B');
        }
        else if (req.url == "/api/status") {
            res.set_header("Content-Type", "application/json");
            res.body = "{\"status\":\"running\",\"protocols\":[\"https\",\"wss\"],\"framework\":\"myframe\",\"multi_protocol\":true}";
        }
        else if (req.url == "/websocket" || req.url == "/ws") {
            // WebSocket握手请求
            std::cout << "[MULTI-HTTP] WebSocket握手请求处理" << std::endl;
            res.body = "WebSocket endpoint - use wss://127.0.0.1:7782/websocket";
        }
        else if (req.url == "/") {
            res.set_header("Content-Type", "text/html");
            res.body = "<html><body>"
                      "<h1>🚀 MyFrame Multi-Protocol Server</h1>"
                      "<p><strong>Framework:</strong> MyFrame</p>"
                      "<p><strong>Protocols:</strong> HTTPS + WSS (Same Port)</p>"
                      "<p><strong>HTTPS端点:</strong> /hello</p>"
                      "<p><strong>大响应:</strong> /big (env MYFRAME_BIG_SIZE controls size)</p>"
                      "<p><strong>WSS端点:</strong> /websocket</p>"
                      "</body></html>";
        }
        else {
            res.status = 404;
            res.body = "404 Not Found - Multi-Protocol Server\nSupported: /hello, /api/status, /websocket, /";
        }
        
        std::cout << "[MULTI-HTTP] 响应: " << res.status << " (" << res.body.length() << " bytes)" << std::endl;
    }
    
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        std::cout << "[MULTI-WSS] 收到WebSocket消息: " << recv.payload << std::endl;
        
        if (recv.payload == "ping") {
            send = WsFrame::text("pong-multi");
            std::cout << "[MULTI-WSS] 发送: pong-multi" << std::endl;
        } 
        else if (recv.payload == "status") {
            send = WsFrame::text("{\"status\":\"running\",\"server\":\"multi-protocol\",\"framework\":\"myframe\"}");
            std::cout << "[MULTI-WSS] 发送状态信息" << std::endl;
        } 
        else if (recv.payload.substr(0, 5) == "echo ") {
            std::string echo_msg = "Multi-Echo: " + recv.payload.substr(5);
            send = WsFrame::text(echo_msg);
            std::cout << "[MULTI-WSS] 发送回显: " << echo_msg << std::endl;
        } 
        else if (recv.payload == "protocols") {
            send = WsFrame::text("HTTPS+WSS on same port via MyFrame MultiProtocolFactory");
            std::cout << "[MULTI-WSS] 发送协议信息" << std::endl;
        }
        else {
            send = WsFrame::text("Multi-Protocol: " + recv.payload);
            std::cout << "[MULTI-WSS] 发送确认: " << recv.payload << std::endl;
        }
    }
    
    void on_connect() override {
        std::cout << "[MULTI] 新连接建立" << std::endl;
    }
    
    void on_disconnect() override {
        std::cout << "[MULTI] 连接断开" << std::endl;
    }
};

std::atomic<bool> g_running(true);
std::unique_ptr<server> g_server;

void signal_handler(int sig) {
    std::cout << "\n停止Multi-Protocol服务器..." << std::endl;
    g_running = false;
    if (g_server) {
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    int port = 7782;  // 默认端口
    bool tls_only = false;
    // 参数解析：第一个数字端口，可选参数 --tls-only 控制模式
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--tls-only") {
            tls_only = true;
        } else {
            // 端口
            port = atoi(argv[i]);
        }
    }
    
    std::cout << "=== MyFrame Multi-Protocol服务器 ===" << std::endl;
    std::cout << "端口: " << port << std::endl;
    std::cout << "协议: " << (tls_only ? "HTTPS + WSS (TlsOnly)" : "HTTP + HTTPS + WS + WSS (Auto)") << std::endl;
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        // 配置TLS证书（用于HTTPS和WSS）
        ssl_config conf;
        conf._cert_file = "/home/rong/myframe/test_certs/server.crt";
        conf._key_file = "/home/rong/myframe/test_certs/server.key";
        conf._protocols = "TLSv1.2:TLSv1.3";
        conf._verify_peer = false;
        tls_set_server_config(conf);
        
        // 创建业务处理器
        std::shared_ptr<MultiProtocolHandler> handler(new MultiProtocolHandler());
        
        // 创建服务器
        g_server.reset(new server(2));
        
        // 绑定监听
        g_server->bind("127.0.0.1", port);
        
        // 使用多协议工厂
        std::shared_ptr<MultiProtocolFactory> factory(
            new MultiProtocolFactory(
                handler.get(),
                tls_only ? MultiProtocolFactory::Mode::TlsOnly : MultiProtocolFactory::Mode::Auto
            )
        );
        
        // 注册工厂
        g_server->set_business_factory(factory);
        
        // 启动服务器
        std::cout << "启动Multi-Protocol服务器..." << std::endl;
        g_server->start();
        
        std::cout << "\n🚀 Multi-Protocol服务器已启动！" << std::endl;
        std::cout << "\n支持的协议组合:" << std::endl;
        std::cout << "┌─────────────────────────────────────────────┐" << std::endl;
        std::cout << "│ HTTP:   curl http://127.0.0.1:" << port << "/hello        │" << std::endl;
        std::cout << "│ HTTPS:  curl -k https://127.0.0.1:" << port << "/hello   │" << std::endl;
        std::cout << "│ WS:     websocat ws://127.0.0.1:" << port << "/websocket  │" << std::endl;
        std::cout << "│ WSS:    websocat wss://127.0.0.1:" << port << "/websocket │" << std::endl;
        std::cout << "│                                             │" << std::endl;
        std::cout << "│ 客户端测试:                                 │" << std::endl;
        std::cout << "│   ./test_https_standalone " << port << "               │" << std::endl;
        std::cout << "│   ./test_wss_standalone " << port << "                 │" << std::endl;
        std::cout << "└─────────────────────────────────────────────┘" << std::endl;
        std::cout << "\n✨ 特性: 同端口自动协议检测" << std::endl;
        std::cout << "按 Ctrl+C 停止服务器" << std::endl;
        
        // 主循环
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "\n正在关闭Multi-Protocol服务器..." << std::endl;
        g_server->stop();
        g_server->join();
        
        std::cout << "Multi-Protocol服务器已停止" << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Multi-Protocol服务器异常: " << e.what() << std::endl;
        return 1;
    }
}
