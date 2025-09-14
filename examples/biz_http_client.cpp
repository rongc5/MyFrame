#include "../core/out_connect.h"
#include "../core/http_req_process.h"
#include "../core/http_client_data_process.h"
#include "../core/base_net_thread.h"
#include <iostream>
#include <regex>
#include <map>

// Simple URL parser for http/https
static bool parse_url(const std::string& url, std::string& scheme, std::string& host, std::string& port, std::string& path) {
    std::regex re(R"((http|https)://([^/ :]+)(:([0-9]+))?(/.*)?)");
    std::smatch m; if (!std::regex_match(url, m, re)) return false;
    scheme = m[1]; host = m[2]; port = m[4].matched? m[4].str(): (scheme=="https"?"443":"80"); path = m[5].matched? m[5].str(): "/"; return true;
}

// This example shows a minimal business client using http_req_process.
// One-shot request, auto-exit after response.
// Note: http_client_data_process::msg_recv_finish() already calls stop_all_thread().
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <http(s)://host[:port]/path> [GET|POST] [body]\n";
        return 1;
    }

    std::string url = argv[1], scheme, host, port, path;
    if (!parse_url(url, scheme, host, port, path)) {
        std::cerr << "Bad URL: " << url << std::endl; return 2;
    }
    std::string method = (argc > 2 ? argv[2] : "GET");
    std::string body   = (argc > 3 ? argv[3] : "");

    base_net_thread th; th.start();

    // Create TCP connection (no TLS for simplicity in this example)
    auto conn = std::make_shared< out_connect<http_req_process> >(host, (unsigned short)atoi(port.c_str()));
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
    proc->set_process(new http_client_data_process(proc, method, host, path, headers, body));

    conn->set_process(proc);
    conn->set_net_container(th.get_net_container());
    std::shared_ptr<base_net_obj> net = conn; th.get_net_container()->push_real_net(net);
    conn->connect();

    // Wait until http_client_data_process stops threads on completion
    th.join_thread();
    return 0;
}

