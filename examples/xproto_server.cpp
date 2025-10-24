#include "../include/server.h"
#include "../include/multi_protocol_factory.h"
#include "../core/app_handler_v2.h"
#include <iostream>

class XProtoHandler : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest& req, myframe::HttpResponse& rsp) override {
        rsp.status = 200; rsp.body = "XPROTO server: http ok"; rsp.set_content_type("text/plain");
    }
    void on_ws(const myframe::WsFrame& recv, myframe::WsFrame& send) override {
        send = myframe::WsFrame::text("XPROTO server: ws ok:" + recv.payload);
    }
    void on_binary(const myframe::BinaryRequest& req, myframe::BinaryResponse& res) override {
        std::cout << "[XPROTO] protocol=" << req.protocol_id
                  << " recv=" << std::string(reinterpret_cast<const char*>(req.payload.data()), req.payload.size())
                  << std::endl;
        res.data = req.payload;
    }
};

int main(int argc, char** argv) {
    std::string ip = "127.0.0.1";
    unsigned short port = 7790;
    if (argc > 1) port = (unsigned short)atoi(argv[1]);

    server srv(2);
    auto handler = std::make_shared<XProtoHandler>();
    auto biz = std::make_shared<MultiProtocolFactory>(handler.get(), MultiProtocolFactory::Mode::Auto);
    srv.bind(ip, port);
    srv.set_business_factory(biz);
    try { srv.start(); srv.join(); } catch (...) { return 2; }
    return 0;
}
