#include "../core/out_connect.h"
#include "../core/http_req_process.h"
#include "../core/http_client_data_process.h"
#include "../core/base_net_thread.h"
#include "../core/tls_out_connect.h"
#include <iostream>
#include <regex>
#include <map>
#include <thread>
#include <chrono>

// Simple URL parser for http/https
static bool parse_url(const std::string& url, std::string& scheme, std::string& host, std::string& port, std::string& path) {
    std::regex re(R"((http|https)://([^/ :]+)(:([0-9]+))?(/.*)?)");
    std::smatch m; if (!std::regex_match(url, m, re)) return false;
    scheme = m[1]; host = m[2]; port = m[4].matched? m[4].str(): (scheme=="https"?"443":"80"); path = m[5].matched? m[5].str(): "/"; return true;
}

// This example shows a minimal business client using http_req_process.
// One-shot request that waits for the response and exits cleanly.
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <http(s)://host[:port]/path> [GET|POST] [body] [--timeout-ms N]\n";
        return 1;
    }

    std::string url = argv[1], scheme, host, port, path;
    if (!parse_url(url, scheme, host, port, path)) {
        std::cerr << "Bad URL: " << url << std::endl; return 2;
    }
    std::string method = (argc > 2 ? argv[2] : "GET");
    std::string body   = (argc > 3 ? argv[3] : "");
    int timeout_ms = 0;
    for (int i=2; i+1<argc; ++i) {
        if (std::string(argv[i]) == "--timeout-ms") timeout_ms = atoi(argv[i+1]);
    }

    base_net_thread th;
    if (!th.start()) {
        std::cerr << "[biz_http_client] Failed to start network thread" << std::endl;
        return 3;
    }

    std::shared_ptr< out_connect<http_req_process> > conn;
#ifdef ENABLE_SSL
    if (scheme == "https") conn = std::make_shared< tls_out_connect<http_req_process> >(host, (unsigned short)atoi(port.c_str()), host);
    else
#endif
    conn = std::make_shared< out_connect<http_req_process> >(host, (unsigned short)atoi(port.c_str()));
    auto proc = new http_req_process(conn);

    // Build headers/body per business need
    std::map<std::string,std::string> headers;
    headers["User-Agent"] = "biz-http-client/1.0";
    if (!body.empty()) headers["Content-Type"] = "application/json";

    // Use the built-in client data_process which will:
    //  - Generate HTTP/1.1 head+body
    //  - Collect response body
    //  - Print status/body summary
    //  - Stop event threads once done (one-shot)
    auto client = new http_client_data_process(proc, method, host, path, headers, body);
    proc->set_process(client);

    conn->set_process(proc);
    conn->set_net_container(th.get_net_container());
    std::shared_ptr<base_net_obj> net = conn; th.get_net_container()->push_real_net(net);
    bool success = false;
    try {
        conn->connect();
        success = client->wait_done(timeout_ms > 0 ? timeout_ms : 0);
    } catch (const std::exception& ex) {
        std::cerr << "[biz_http_client] request failed: " << ex.what() << std::endl;
    }
    if (!success) {
        if (timeout_ms > 0) {
            std::cerr << "[biz_http_client] timeout after " << timeout_ms << " ms" << std::endl;
        } else {
            std::cerr << "[biz_http_client] request did not complete" << std::endl;
        }
    }
    th.stop();
    th.join_thread();
    return success ? 0 : 4;
}
