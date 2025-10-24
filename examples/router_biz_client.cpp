#include "../core/base_net_thread.h"
#include "../core/client_conn_router.h"
#include "../core/app_handler_v2.h"
#include <iostream>
#include <map>
#include <regex>
#include <thread>
#include <chrono>

static std::string scheme_of(const std::string& url) {
    auto p = url.find("://");
    if (p == std::string::npos) return std::string();
    std::string s = url.substr(0, p);
    for (auto& c : s) c = (char)tolower(c);
    return s;
}

class SimpleHandler : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest& req, myframe::HttpResponse& rsp) override { (void)req; (void)rsp; }
    void on_ws(const myframe::WsFrame& recv, myframe::WsFrame& send) override {
        std::cout << "[ws] recv opcode=" << (int)recv.opcode << ", size=" << recv.payload.size() << std::endl;
        if (recv.opcode == myframe::WsFrame::TEXT) std::cout << "[ws] text: " << recv.payload << std::endl;
        // echo
        send = myframe::WsFrame::text(std::string("echo: ") + recv.payload);
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <URL> [--method GET|POST] [--H 'K: V']... [--data BODY] [--timeout-ms N]" << std::endl;
        return 1;
    }
    std::string url = argv[1];
    std::string method = "GET"; std::string body; int timeout_ms = 0;
    std::map<std::string,std::string> hdrs;
    for (int i=2; i<argc; ++i) {
        std::string a = argv[i];
        if (a == "--method" && i+1<argc) { method = argv[++i]; }
        else if (a == "--data" && i+1<argc) { body = argv[++i]; }
        else if (a == "--H" && i+1<argc) {
            std::string hv = argv[++i]; auto p = hv.find(':');
            if (p!=std::string::npos) { std::string k = hv.substr(0,p); std::string v = hv.substr(p+1); if (!v.empty() && v[0]==' ') v.erase(0,1); hdrs[k]=v; }
        } else if (a == "--timeout-ms" && i+1<argc) { timeout_ms = atoi(argv[++i]); }
    }

    std::string sch = scheme_of(url);
    base_net_thread th; th.start();
    ClientConnRouter router; register_default_client_builders(router);

    SimpleHandler handler;
    ClientBuildCtx ctx; ctx.container = th.get_net_container(); ctx.method = method; ctx.headers = hdrs; ctx.body = body;
    if (sch == "ws" || sch == "wss") ctx.handler = &handler;

    auto net = router.create_and_enqueue(url, ctx);
    if (!net) { std::cerr << "Create connection failed for: " << url << std::endl; return 2; }

    if ((sch == "ws" || sch == "wss") && timeout_ms > 0) {
        std::thread([timeout_ms]{ std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms)); base_thread::stop_all_thread(); }).detach();
    }

    th.join_thread();
    return 0;
}
