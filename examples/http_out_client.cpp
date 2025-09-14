#include "../core/out_connect.h"
#include "../core/tls_out_connect.h"
#include "../core/http_req_process.h"
#include "../core/http_client_data_process.h"
#include "../core/base_net_thread.h"
#include <iostream>
#include <regex>
#include <thread>
#include <chrono>

static bool parse_url(const std::string& url, std::string& scheme, std::string& host, std::string& port, std::string& path) {
    std::regex re(R"((http|https)://([^/ :]+)(:([0-9]+))?(/.*)?)");
    std::smatch m; if (!std::regex_match(url, m, re)) return false;
    scheme = m[1]; host = m[2]; port = m[4].matched? m[4].str(): (scheme=="https"?"443":"80"); path = m[5].matched? m[5].str(): "/"; return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "Usage: " << argv[0] << " <http(s)://host[:port]/path> [--timeout-ms N]\n"; return 1; }
    std::string url = argv[1], scheme, host, port, path; if (!parse_url(url, scheme, host, port, path)) { std::cerr << "bad url\n"; return 2; }
    int timeout_ms = 0; for (int i=2; i+1<argc; ++i) { if (std::string(argv[i])=="--timeout-ms") timeout_ms = atoi(argv[i+1]); }
    base_net_thread th; th.start();
    std::shared_ptr< out_connect<http_req_process> > conn;
#ifdef ENABLE_SSL
    if (scheme == "https") conn = std::make_shared< tls_out_connect<http_req_process> >(host, (unsigned short)atoi(port.c_str()), host);
    else
#endif
    conn = std::make_shared< out_connect<http_req_process> >(host, (unsigned short)atoi(port.c_str()));
    auto proc = new http_req_process(conn);
    std::map<std::string,std::string> headers; headers["User-Agent"]="myframe-http1-client";
    proc->set_process(new http_client_data_process(proc, "GET", host, path, headers, std::string()));
    conn->set_process(proc);
    conn->set_net_container(th.get_net_container());
    std::shared_ptr<base_net_obj> net = conn; th.get_net_container()->push_real_net(net);
    conn->connect();
    if (timeout_ms > 0) { std::thread([timeout_ms]{ std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms)); base_thread::stop_all_thread(); }).detach(); }
    th.join_thread();
    return 0;
}
