#include "../core/client_iface.h"
#include <iostream>
#include <map>

// Simple HTTP/2 client example using MyFrame client_iface
// Supports URLs like:
//   - https://host[:port]/path  (ALPN h2 if available)
//   - h2://host[:port]/path     (force use of HTTP/2 client)

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <url>\n";
        std::cerr << "Examples:\n";
        std::cerr << "  " << argv[0] << " h2://127.0.0.1:7779/hello\n";
        std::cerr << "  " << argv[0] << " https://127.0.0.1:7779/hello\n";
        return 1;
    }
    std::string url = argv[1];

    RequestRouter router;
    // HTTP/1.1 for plain http
    router.register_factory("http", make_http1_factory());
    // HTTP/2 for https (ALPN will request h2 and fall back internally if needed)
    router.register_factory("https", make_http2_factory());
    // Non-standard scheme to explicitly force the HTTP/2 client path
    router.register_factory("h2", make_http2_factory());

    std::map<std::string,std::string> headers;
    headers["user-agent"] = "myframe-h2-client";
    HttpResult r = router.get(url, headers);

    std::cout << "status: " << r.status << "\n";
    for (auto& kv : r.headers) std::cout << kv.first << ": " << kv.second << "\n";
    std::cout << "\n" << r.body << std::endl;
    return (r.status == 200) ? 0 : 2;
}

