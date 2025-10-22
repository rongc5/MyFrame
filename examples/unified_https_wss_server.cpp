// MyFrame Unified Protocol Framework - HTTPS/WSS Server Demo
// ֧�� HTTP+HTTPS �� WebSocket+WSS ��ͳһЭ����
// ��ʾ�����ͬһ�˿���֧�ּ��ܺͷǼ��ܵĶ�Э��ͨ��

#include "server.h"
#include "unified_protocol_factory.h"
#include "app_handler_v2.h"
#include "ssl_context.h"
#include <iostream>
#include <sstream>
#include <ctime>
#include <cstring>

using namespace myframe;

// ============================================================================
// HTTPS/WSS ������ - Level 1 Application Handler
// ============================================================================

class SecureApplicationHandler : public IApplicationHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        std::cout << "[HTTP] " << req.method << " " << req.url
                  << " (protocol: " << req.version << ")" << std::endl;

        if (req.url == "/" || req.url == "/index.html") {
            res.set_html(R"HTML(
<!DOCTYPE html>
<html>
<head>
  <title>MyFrame HTTPS/WSS Server</title>
  <style>
    body { font-family: Arial; margin: 20px; }
    .section { margin: 20px 0; padding: 10px; border: 1px solid #ccc; }
  </style>
</head>
<body>
  <h1>MyFrame Unified Protocol Server</h1>

  <div class="section">
    <h2>HTTP/HTTPS Tests</h2>
    <p><a href="/api/hello">GET /api/hello</a></p>
    <p><a href="/api/info">GET /api/info</a></p>
    <p><a href="/api/version">GET /api/version</a></p>
  </div>

  <div class="section">
    <h2>WebSocket/WSS Test</h2>
    <button onclick="connect()">Connect WebSocket</button>
    <button onclick="send()">Send Message</button>
    <button onclick="disconnect()">Disconnect</button>
    <textarea id="log" style="width:100%;height:200px;" readonly></textarea>
  </div>

  <script>
    var ws = null;

    function log(msg) {
      document.getElementById('log').value += msg + '\n';
      document.getElementById('log').scrollTop = document.getElementById('log').scrollHeight;
    }

    function connect() {
      if (ws) return;
      var protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      ws = new WebSocket(protocol + '//' + window.location.host + '/ws');
      ws.onopen = function() { log('[Connected]'); };
      ws.onmessage = function(e) { log('RX: ' + e.data); };
      ws.onerror = function(e) { log('[Error] ' + e); };
      ws.onclose = function() { log('[Disconnected]'); ws = null; };
    }

    function send() {
      if (ws && ws.readyState === WebSocket.OPEN) {
        var msg = 'Hello from ' + new Date().toLocaleTimeString();
        ws.send(msg);
        log('TX: ' + msg);
      }
    }

    function disconnect() {
      if (ws) {
        ws.close();
      }
    }

    // Auto-connect for demo
    window.onload = function() { connect(); };
  </script>
</body>
</html>)HTML");
        } else if (req.url == "/api/hello") {
            res.set_json(R"({"message":"Hello from HTTPS/WSS Server","protocol":")" +
                        req.version + R"("})");
        } else if (req.url == "/api/info") {
            std::stringstream json;
            json << R"({"info":{"time":)" << std::time(nullptr)
                 << R"(,"method":")" << req.method << R"("}})";
            res.set_json(json.str());
        } else if (req.url == "/api/version") {
            res.set_json(R"({"version":"1.0.0","framework":"MyFrame v2.0"})");
        } else {
            res.status = 404;
            res.reason = "Not Found";
            res.set_text("404 Not Found");
        }
    }

    void on_ws(const WsFrame& recv, WsFrame& send) override {
        std::cout << "[WS] Received: " << recv.payload.substr(0, 50) << std::endl;

        if (recv.opcode == WsFrame::TEXT) {
            // ������Ϣ������ʱ���
            std::stringstream response;
            response << "Echo @ " << std::time(nullptr) << ": " << recv.payload;
            send = WsFrame::text(response.str());
        }
    }

    void on_connect(const ConnectionInfo& info) override {
        std::cout << "[CONNECT] " << info.remote_ip << ":" << info.remote_port << std::endl;
    }

    void on_disconnect() override {
        std::cout << "[DISCONNECT]" << std::endl;
    }

    void on_error(const std::string& error) override {
        std::cout << "[ERROR] " << error << std::endl;
    }
};

// ============================================================================
// ������
// ============================================================================

int main(int argc, char* argv[]) {
    unsigned short port = 443;  // Ĭ�� HTTPS �˿�
    bool use_tls = true;

    if (argc > 1) {
        port = static_cast<unsigned short>(std::atoi(argv[1]));
    }

    if (argc > 2) {
        std::string mode = argv[2];
        use_tls = (mode != "plain");
    }

    std::cout << "======================================" << std::endl;
    std::cout << "  MyFrame HTTPS/WSS Server Demo" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "Listening on: 0.0.0.0:" << port << std::endl;
    std::cout << "Mode: " << (use_tls ? "HTTPS/WSS (TLS)" : "HTTP/WS (Plain)") << std::endl;

    if (use_tls) {
        // ���� TLS��ʹ�ò���֤�飩
        // �����������У�Ӧʹ����ʵ��֤�����Կ
        ssl_config conf;

        // ���Դӻ���������Ĭ��·����ȡ֤��
        const char* cert_file = std::getenv("MYFRAME_SSL_CERT");
        const char* key_file = std::getenv("MYFRAME_SSL_KEY");

        if (!cert_file) cert_file = "tests/common/cert/server.crt";
        if (!key_file) key_file = "tests/common/cert/server.key";

        conf._cert_file = cert_file;
        conf._key_file = key_file;

        std::cout << "TLS Config:" << std::endl;
        std::cout << "  Certificate: " << conf._cert_file << std::endl;
        std::cout << "  Private Key: " << conf._key_file << std::endl;

        tls_set_server_config(conf);
        std::cout << "[TLS] Configuration set" << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Access point:" << std::endl;
    std::string protocol = use_tls ? "https" : "http";
    std::cout << "  " << protocol << "://127.0.0.1:" << port << "/" << std::endl;
    std::cout << std::endl;

    try {
        // ����������
        server srv(2);  // 1 listen thread + 1 worker thread

        // ��������
        auto factory = std::make_shared<UnifiedProtocolFactory>();

        // ����������
        auto handler = std::make_shared<SecureApplicationHandler>();

        // ע�ᴦ������ͬʱ֧�� HTTP/HTTPS �� WS/WSS��
        factory->register_http_handler(handler.get())
                .register_ws_handler(handler.get());

        // ���÷�����
        srv.bind("0.0.0.0", port);
        srv.set_business_factory(factory);

        // ����
        srv.start();

        std::cout << "Press Ctrl+C to stop..." << std::endl;
        std::cout << std::endl;

        // �ȴ�
        srv.join();

        std::cout << "Server stopped." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
