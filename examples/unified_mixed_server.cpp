// MyFrame Unified Protocol Architecture - Mixed HTTP + WebSocket Example
// Demonstrates Level 1 API with multiple protocols

#include "../include/server.h"
#include "../core/unified_protocol_factory.h"
#include "../core/app_handler_v2.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>

using namespace myframe;

// ============================================================================
// Mixed Protocol Handler (Level 1)
// 同时处理 HTTP 和 WebSocket 请求
// ============================================================================

class MixedProtocolHandler : public IApplicationHandler {
public:
    // HTTP 处理 - 提供 WebSocket 客户端页面
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        std::cout << "[HTTP] " << req.method << " " << req.url << std::endl;

        if (req.url == "/") {
            // 提供包含 WebSocket 客户端的 HTML 页面
            res.set_html(
                "<html><head><title>WebSocket Demo</title></head><body>"
                "<h1>MyFrame Unified Protocol Demo</h1>"
                "<h2>WebSocket Echo Test</h2>"
                "<div>"
                "  <input type='text' id='msg' placeholder='Type a message' style='width:300px'/>"
                "  <button onclick='send()'>Send</button>"
                "  <button onclick='connect()'>Connect</button>"
                "  <button onclick='disconnect()'>Disconnect</button>"
                "</div>"
                "<div id='status' style='margin-top:10px; color:blue'>Not connected</div>"
                "<div id='messages' style='margin-top:10px; border:1px solid #ccc; padding:10px; height:200px; overflow-y:auto'></div>"
                "<script>"
                "var ws = null;"
                "function connect() {"
                "  ws = new WebSocket('ws://' + location.host + '/ws');"
                "  ws.onopen = function() { document.getElementById('status').innerHTML = 'Connected'; document.getElementById('status').style.color = 'green'; };"
                "  ws.onclose = function() { document.getElementById('status').innerHTML = 'Disconnected'; document.getElementById('status').style.color = 'red'; };"
                "  ws.onmessage = function(e) { addMessage('Server: ' + e.data); };"
                "  ws.onerror = function(e) { addMessage('Error: ' + e); };"
                "}"
                "function disconnect() { if(ws) ws.close(); }"
                "function send() {"
                "  var msg = document.getElementById('msg').value;"
                "  if(ws && ws.readyState === 1) { ws.send(msg); addMessage('You: ' + msg); document.getElementById('msg').value = ''; }"
                "  else { alert('Not connected'); }"
                "}"
                "function addMessage(msg) {"
                "  var div = document.getElementById('messages');"
                "  div.innerHTML += '<div>' + msg + '</div>';"
                "  div.scrollTop = div.scrollHeight;"
                "}"
                "document.getElementById('msg').addEventListener('keypress', function(e) { if(e.key === 'Enter') send(); });"
                "</script>"
                "</body></html>"
            );
        }
        else if (req.url == "/api/status") {
            res.set_json("{\"status\":\"running\",\"protocols\":[\"HTTP\",\"WebSocket\"]}");
        }
        else {
            res.status = 404;
            res.set_text("404 Not Found");
        }
    }

    // WebSocket 处理 - Echo Server
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        std::cout << "[WebSocket] Received: opcode=" << (int)recv.opcode
                  << ", payload_size=" << recv.payload.size() << std::endl;

        if (recv.opcode == WsFrame::TEXT || recv.opcode == WsFrame::BINARY) {
            // Echo back the message
            send = recv;
            std::cout << "[WebSocket] Echoing back: " << recv.payload << std::endl;
        }
        else if (recv.opcode == WsFrame::CLOSE) {
            std::cout << "[WebSocket] Client requested close" << std::endl;
            send.opcode = WsFrame::CLOSE;
            send.payload = "";
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

    std::cout << "=== MyFrame Mixed Protocol Demo ===" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "Protocols: HTTP + WebSocket" << std::endl;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        // 创建服务器
        server srv(2);  // 1 listen + 1 worker
        g_server = &srv;

        // 创建业务处理器（Level 1）
        auto handler = std::make_shared<MixedProtocolHandler>();

        // 创建统一协议工厂
        auto factory = std::make_shared<UnifiedProtocolFactory>();

        // 注册 HTTP 和 WebSocket 处理器（Level 1）
        factory->register_http_handler(handler.get());
        factory->register_ws_handler(handler.get());

        // 配置服务器
        srv.bind("0.0.0.0", port);
        srv.set_business_factory(factory);

        // 启动
        srv.start();

        std::cout << "\n✅ Server started!" << std::endl;
        std::cout << "\nOpen in browser: http://127.0.0.1:" << port << "/" << std::endl;
        std::cout << "Or test with:" << std::endl;
        std::cout << "  curl http://127.0.0.1:" << port << "/" << std::endl;
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
