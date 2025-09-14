#include "../core/base_net_thread.h"
#include "../core/client_conn_router.h"
#include <iostream>
#include <thread>
#include <regex>

static std::string scheme_of(const std::string& url) {
    auto p = url.find("://");
    if (p == std::string::npos) return std::string();
    std::string s = url.substr(0, p);
    for (auto& c : s) c = (char)tolower(c);
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <http|https|ws|wss|h2 URL> [--timeout-ms N] [--no-accept-encoding] [--user-agent UA]" << std::endl;
        return 1;
    }
    std::string url = argv[1];
    int timeout_ms = 0;
    bool no_ae = false;
    std::string user_agent;
    for (int i=2; i+1<argc; ++i) {
        if (std::string(argv[i]) == "--timeout-ms") timeout_ms = atoi(argv[i+1]);
    }
    for (int i=2; i<argc; ++i) {
        if (std::string(argv[i]) == "--no-accept-encoding") no_ae = true;
        if (std::string(argv[i]) == "--user-agent" && i+1<argc) { user_agent = argv[i+1]; ++i; }
    }

    base_net_thread th; th.start();
    ClientConnRouter router; register_default_client_builders(router);

    ClientBuildCtx ctx; ctx.container = th.get_net_container();
    if (no_ae) setenv("MYFRAME_NO_DEFAULT_AE", "1", 1);
    if (!user_agent.empty()) ctx.headers["User-Agent"] = user_agent;
    auto net = router.create_and_enqueue(url, ctx);
    if (!net) {
        std::cerr << "Create connection failed for: " << url << std::endl;
        return 2;
    }

    // Optional timeout for all schemes to avoid hanging during debugging.
    // For HTTP-like clients, normal completion will stop threads earlier.
    if (timeout_ms > 0) {
        std::thread([timeout_ms]{
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            base_thread::stop_all_thread();
        }).detach();
    }

    th.join_thread();
    return 0;
}
