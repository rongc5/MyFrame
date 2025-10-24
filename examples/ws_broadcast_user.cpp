#include "server.h"
#include "multi_protocol_factory.h"
#include "app_handler_v2.h"
#include "../core/ws_push_hub.h"
#include <sstream>
#include <iostream>

class WsBroadcastUserApp : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest& req, myframe::HttpResponse& res) override {
        if (req.url.rfind("/login", 0) == 0) {
            // /login?user=alice
            auto pos = req.url.find('?');
            std::string user = "";
            if (pos != std::string::npos) {
                std::string qs = req.url.substr(pos + 1);
                auto p = parse_query(qs);
                auto it = p.find("user"); if (it != p.end()) user = it->second;
            }
            if (user.empty()) user = "guest";
            std::ostringstream cookie;
            cookie << "username=" << user << "|dummy; Path=/; HttpOnly=false; SameSite=Lax";
            res.status = 200; res.set_content_type("text/html; charset=utf-8");
            res.headers["Set-Cookie"] = cookie.str();
            res.body = std::string("<html><body>login ok as ") + user + "</body></html>";
            return;
        }
        if (req.url.rfind("/push", 0) == 0) {
            // /push?user=alice&msg=hello
            auto pos = req.url.find('?');
            std::string user, msg;
            if (pos != std::string::npos) {
                auto q = parse_query(req.url.substr(pos + 1));
                auto iu = q.find("user"); if (iu != q.end()) user = iu->second;
                auto im = q.find("msg"); if (im != q.end()) msg = im->second;
            }
            if (!user.empty() && !msg.empty()) {
                std::string payload = std::string("{\"type\":\"demo.push\",\"msg\":\"") + msg + "\"}";
                WsPushHub::Instance().BroadcastToUser(user, payload);
                res.status = 200; res.set_content_type("application/json");
                res.body = "{\"ok\":true}";
            } else {
                res.status = 400; res.set_content_type("application/json");
                res.body = "{\"ok\":false,\"err\":\"missing user/msg\"}";
            }
            return;
        }
        if (req.url == "/" || req.url == "/index") {
            res.status = 200; res.set_content_type("text/html; charset=utf-8");
            res.body =
                "<html><body>"
                "<h3>WS User Broadcast Demo</h3>"
                "<p>1) Set cookie: <code>/login?user=alice</code></p>"
                "<p>2) Connect WebSocket: <code>ws://127.0.0.1:7782/websocket</code></p>"
                "<p>3) Send push: <code>/push?user=alice&msg=hello</code></p>"
                "</body></html>";
            return;
        }
        // 404
        res.status = 404; res.set_content_type("text/plain; charset=utf-8");
        res.body = "Not Found";
    }

    void on_ws(const myframe::WsFrame& recv, myframe::WsFrame& send) override {
        if (recv.opcode == myframe::WsFrame::TEXT && recv.payload == "ping") {
            send = myframe::WsFrame::text("pong");
        } else {
            send = myframe::WsFrame::text(std::string("echo:") + recv.payload);
        }
    }

private:
    static std::map<std::string, std::string> parse_query(const std::string& qs) {
        std::map<std::string, std::string> m;
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            std::string pair = qs.substr(i, amp == std::string::npos ? qs.size() - i : amp - i);
            size_t eq = pair.find('=');
            if (eq != std::string::npos) m[pair.substr(0, eq)] = pair.substr(eq + 1);
            if (amp == std::string::npos) break; i = amp + 1;
        }
        return m;
    }
};

int main(int argc, char** argv) {
    int port = 7782;
    if (argc > 1) port = std::atoi(argv[1]);
    WsBroadcastUserApp app;
    auto factory = std::make_shared<MultiProtocolFactory>(&app, MultiProtocolFactory::Mode::PlainOnly);
    server s(1);
    s.bind("0.0.0.0", (unsigned short)port);
    s.set_business_factory(factory);
    s.start();
    std::cout << "User broadcast demo running on http://127.0.0.1:" << port << "\n";
    std::cout << "Login:  curl -s http://127.0.0.1:" << port << "/login?user=alice\n";
    std::cout << "WS:     websocat ws://127.0.0.1:" << port << "/websocket\n";
    std::cout << "Push:   curl -s 'http://127.0.0.1:" << port << "/push?user=alice&msg=hello'\n";
    s.join();
    return 0;
}
