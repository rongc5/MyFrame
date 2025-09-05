#include "../core/client_conn_router.h"
#include "../core/base_net_thread.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <url>\n";
        std::cerr << "Example: " << argv[0] << " http://127.0.0.1:7782/hello\n";
        return 1;
    }
    std::string url = argv[1];
    base_net_thread th; th.start();
    ClientConnRouter router; register_default_client_builders(router);
    ClientBuildCtx ctx; ctx.container = th.get_net_container();
    ctx.method = "GET"; ctx.headers["User-Agent"] = "myframe-router-client";
    std::shared_ptr<base_net_obj> net = router.create_and_enqueue(url, ctx);
    if (!net) { std::cerr << "no builder for url: " << url << "\n"; return 2; }
    th.join_thread();
    return 0;
}


