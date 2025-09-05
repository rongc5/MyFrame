#include "../include/server.h"
#include "../include/multi_protocol_factory.h"
#include "../core/app_handler.h"
#include <iostream>

class XProtoHandler : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& rsp) override {
        rsp.status = 200; rsp.body = "XPROTO server: http ok"; rsp.set_content_type("text/plain");
    }
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        send = WsFrame::text("XPROTO server: ws ok:" + recv.payload);
    }
    void on_custom(uint32_t protocol, const char* data, std::size_t len) override {
        std::cout << "[XPROTO] protocol=" << protocol << " recv=" << std::string(data, len) << std::endl;
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
