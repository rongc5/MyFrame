// MyFrame Unified Protocol Framework - Level 2 Demo
// ��ʾ Level 2 Protocol Context Handler �ĸ߼�����
// ����: �첽��Ӧ����ʽ���䡢״̬���������ӿ��Ƶ�

#include "server.h"
#include "unified_protocol_factory.h"
#include "protocol_context.h"
#include <iostream>
#include <memory>
#include <sstream>
#include <cstdint>
#include <ctime>
#include <thread>

using namespace myframe;

// ============================================================================
// Level 2: �߼� HTTP ������ - ֧���첽��Ӧ
// ============================================================================

class AdvancedHttpHandler : public IProtocolHandler {
public:
    void on_http_request(HttpContext& ctx) override {
        const auto& req = ctx.request();
        auto& res = ctx.response();

        std::cout << "[HTTP] " << req.method << " " << req.url << std::endl;

        // 为每条连接维护计数和首次访问时间，直接编码在 user_data 的 void* 中
        auto raw_count = reinterpret_cast<std::uintptr_t>(ctx.get_user_data("request_count"));
        uint32_t request_count = static_cast<uint32_t>(raw_count);
        request_count++;
        ctx.set_user_data("request_count", reinterpret_cast<void*>(static_cast<std::uintptr_t>(request_count)));

        auto raw_first = reinterpret_cast<std::uintptr_t>(ctx.get_user_data("first_seen"));
        std::time_t first_seen;
        if (raw_first == 0) {
            first_seen = std::time(nullptr);
            ctx.set_user_data("first_seen", reinterpret_cast<void*>(static_cast<std::uintptr_t>(first_seen)));
        } else {
            first_seen = static_cast<std::time_t>(raw_first);
        }

        if (req.url == "/") {
            res.set_html(R"(
<!DOCTYPE html>
<html>
<head><title>MyFrame Level 2 Demo</title></head>
<body>
  <h1>Welcome to MyFrame Unified Protocol Framework</h1>
  <p><a href="/api/hello">GET /api/hello</a></p>
  <p><a href="/api/data">GET /api/data (async)</a></p>
  <p><a href="/api/status">GET /api/status</a></p>
  <p><a href="/ws">WebSocket Demo</a></p>
</body>
</html>)");
        } else if (req.url == "/api/hello") {
            res.set_json(R"({"message":"Hello from Level 2 HTTP Context Handler"})");
        } else if (req.url == "/api/data") {
            // �첽��Ӧʾ��
            // 需要在异步回调中再次访问上下文时，通过消息回到网络线程
            ctx.async_response([&ctx]() {
                // ģ�ⳤʱ����������ݿ��ѯ�ȣ�
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                auto timestamp = std::time(nullptr);
                // 通过 HttpContextTaskMessage 回到网络线程，安全访问上下文
                auto task = std::make_shared<HttpContextTaskMessage>(
                    [timestamp](HttpContext& context) {
                        auto async_raw = reinterpret_cast<std::uintptr_t>(context.get_user_data("request_count"));
                        uint32_t seen = static_cast<uint32_t>(async_raw);
                        std::string json = std::string(R"({"data":"Async response from HTTP handler","timestamp":)") +
                                            std::to_string(timestamp) +
                                            R"(,"requests_on_connection":)" + std::to_string(seen) + "}";
                        context.response().set_json(json);
                        context.complete_async_response();
                    });
                ctx.send_msg(task);
            });
        } else if (req.url == "/api/status") {
            std::stringstream json;
            json << R"({"status":"ok","time":)" << std::time(nullptr)
                 << R"(,"requests_on_connection":)" << request_count
                 << R"(,"first_seen":)" << first_seen << R"(})";
            res.set_json(json.str());
        } else if (req.url == "/ws") {
            res.set_html(R"HTML(
<!DOCTYPE html>
<html>
<head><title>WebSocket Test</title></head>
<body>
  <h1>WebSocket Echo Test</h1>
  <textarea id="log" style="width:100%;height:300px;"></textarea><br/>
  <input id="msg" type="text" placeholder="Enter message"/>
  <button onclick="send()">Send</button>
  <script>
    var ws = new WebSocket('ws://127.0.0.1:8080/ws');
    ws.onopen = function() { log('Connected'); };
    ws.onmessage = function(e) { log('Received: ' + e.data); };
    ws.onclose = function() { log('Disconnected'); };
    function send() {
      var msg = document.getElementById('msg').value;
      ws.send(msg);
      document.getElementById('msg').value = '';
    }
    function log(msg) {
      document.getElementById('log').value += msg + '\n';
    }
  </script>
</body>
</html>)HTML");
        } else {
            res.status = 404;
            res.reason = "Not Found";
            res.set_text("404 Not Found");
        }
    }

    void on_connect(const ConnectionInfo& info) override {
        std::cout << "[CONNECT] " << info.remote_ip << ":" << info.remote_port
                  << " (id=" << info.connection_id << ")" << std::endl;
    }

    void on_disconnect() override {
        std::cout << "[DISCONNECT]" << std::endl;
    }

    void on_error(const std::string& error) override {
        std::cout << "[ERROR] " << error << std::endl;
    }
};

// ============================================================================
// Level 2: �߼� WebSocket ������ - ֧��״̬�����Ϳ���
// ============================================================================

class AdvancedWsHandler : public IProtocolHandler {
public:
    void on_ws_frame(WsContext& ctx) override {
        const auto& frame = ctx.frame();

        // ״̬����ʾ��
        auto raw_count = reinterpret_cast<std::uintptr_t>(ctx.get_user_data("msg_count"));
        uint32_t count = static_cast<uint32_t>(raw_count);
        count++;
        ctx.set_user_data("msg_count", reinterpret_cast<void*>(static_cast<std::uintptr_t>(count)));

        std::cout << "[WS] Received message #" << count << ": "
                  << frame.payload.substr(0, 50) << std::endl;

        if (frame.opcode == WsFrame::TEXT) {
            // �����ı���Ϣ
            std::string response = "Echo: " + frame.payload +
                                 " (msg#" + std::to_string(count) + ")";
            ctx.send_text(response);

            // �Զ��ظ� PING
            if (frame.payload == "ping") {
                ctx.send_ping();
            }
        } else if (frame.opcode == WsFrame::BINARY) {
            // ������������Ϣ
            ctx.send_binary(frame.payload.data(), frame.payload.size());
        }
    }

    void on_connect(const ConnectionInfo& info) override {
        std::cout << "[WS CONNECT] " << info.remote_ip << ":" << info.remote_port << std::endl;
    }

    void on_disconnect() override {
        std::cout << "[WS DISCONNECT]" << std::endl;
    }
};

// ============================================================================
// ������
// ============================================================================

int main(int argc, char* argv[]) {
    unsigned short port = 8080;
    if (argc > 1) {
        port = static_cast<unsigned short>(std::atoi(argv[1]));
    }

    std::cout << "Starting MyFrame Level 2 Demo Server..." << std::endl;
    std::cout << "Listening on 0.0.0.0:" << port << std::endl;
    std::cout << "Open browser: http://127.0.0.1:" << port << "/" << std::endl;

    // ����������
    server srv(2);  // 1 listen thread + 1 worker thread

    // ��������
    auto factory = std::make_shared<UnifiedProtocolFactory>();

    // ����������
    auto http_handler = std::make_shared<AdvancedHttpHandler>();
    auto ws_handler = std::make_shared<AdvancedWsHandler>();

    // ע�� Level 2 Context Handler
    factory->register_http_context_handler(http_handler.get())
            .register_ws_context_handler(ws_handler.get());

    // ���÷�����
    srv.bind("0.0.0.0", port);
    srv.set_business_factory(factory);

    // ����
    srv.start();

    // �ȴ�
    std::cout << "Press Ctrl+C to stop..." << std::endl;
    srv.join();

    std::cout << "Server stopped." << std::endl;
    return 0;
}
