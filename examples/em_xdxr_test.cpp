// Minimal repro: 用 myframe http_client 命中 EastMoney 长 URL + filter param.
// 目的: 隔离 stockbridge 里 daemon httpGet 100% fail 的具体原因.
//
// 同 URL 用 curl 工作, biz_http_client 也 exit=0 (success), 但 stockbridge 失败.
// 区别假设:
//   1. daemon httpGet 传 resolved IP, biz_http_client 传 hostname
//   2. headers 不同 (daemon 有 Connection:close + Host)
//
// 用法:
//   ./em_xdxr_test [http(s)://...]    # 默认跑 EastMoney RPT_SHAREBONUS_DET
//   stdout: full response body 或 error message
#include "../core/out_connect.h"
#include "../core/tls_out_connect.h"
#include "../core/http_req_process.h"
#include "../core/http_client_data_process.h"
#include "../core/base_net_thread.h"

#include <iostream>
#include <map>
#include <regex>
#include <string>

static bool parse_url(const std::string& url,
                      std::string& scheme, std::string& host,
                      unsigned short& port, std::string& path) {
    std::regex re(R"((http|https)://([^/ :]+)(:([0-9]+))?(/.*)?)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) return false;
    scheme = m[1].str();
    host = m[2].str();
    port = m[4].matched ? static_cast<unsigned short>(std::stoi(m[4].str()))
                        : static_cast<unsigned short>(scheme == "https" ? 443 : 80);
    path = m[5].matched ? m[5].str() : "/";
    return true;
}

int main(int argc, char** argv) {
    // 默认 URL = EastMoney xdxr date-range query (长 URL + filter param)
    std::string url = (argc > 1) ? argv[1] :
        "http://datacenter-web.eastmoney.com/api/data/v1/get"
        "?reportName=RPT_SHAREBONUS_DET"
        "&columns=SECURITY_CODE,EX_DIVIDEND_DATE,PRETAX_BONUS_RMB,IT_RATIO,BONUS_RATIO,ASSIGN_PROGRESS"
        "&filter=%28EX_DIVIDEND_DATE%3E%3D%272026-06-01%27%29"
        "%28ASSIGN_PROGRESS%3D%22%E5%AE%9E%E6%96%BD%E5%88%86%E9%85%8D%22%29"
        "&pageNumber=1&pageSize=5"
        "&sortColumns=EX_DIVIDEND_DATE&sortTypes=-1";

    std::string scheme, host, path;
    unsigned short port;
    if (!parse_url(url, scheme, host, port, path)) {
        std::cerr << "[em_xdxr_test] bad URL\n"; return 1;
    }

    std::cerr << "[em_xdxr_test] scheme=" << scheme << " host=" << host
              << " port=" << port << "\n";
    std::cerr << "[em_xdxr_test] path_len=" << path.size() << "\n";
    std::cerr << "[em_xdxr_test] path=" << path.substr(0, 200)
              << (path.size() > 200 ? "..." : "") << "\n";

    base_net_thread th;
    if (!th.start()) {
        std::cerr << "[em_xdxr_test] base_net_thread start failed\n"; return 2;
    }

    // ── 直接传 hostname (不 resolveHost) — 跟 biz_http_client 一样 ──
    std::shared_ptr<out_connect<http_req_process>> conn;
#ifdef ENABLE_SSL
    if (scheme == "https") {
        conn = std::make_shared<tls_out_connect<http_req_process>>(host, port, host);
    } else
#endif
    {
        conn = std::make_shared<out_connect<http_req_process>>(host, port);
    }

    auto proc = new http_req_process(conn);

    // ── headers 跟 stockbridge daemon 一样 ──
    std::map<std::string, std::string> headers;
    headers["Host"] = host;
    headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
    headers["Connection"] = "close";
    headers["Accept-Encoding"] = "identity";
    headers["Referer"] = "https://emweb.securities.eastmoney.com/";

    auto client = new http_client_data_process(proc, "GET", host, path, headers, "");
    proc->set_process(client);

    conn->set_process(proc);
    conn->set_net_container(th.get_net_container());
    std::shared_ptr<base_net_obj> net = conn;
    th.get_net_container()->push_real_net(net);

    bool success = false;
    try {
        conn->connect();
        success = client->wait_done(8000);
    } catch (const std::exception& ex) {
        std::cerr << "[em_xdxr_test] connect/wait exception: " << ex.what() << "\n";
    }

    int status = client->status();
    const std::string& body = client->response_body();
    std::cerr << "[em_xdxr_test] success=" << success
              << " status=" << status
              << " body_len=" << body.size() << "\n";

    if (!body.empty()) {
        std::cout << body.substr(0, 1000)
                  << (body.size() > 1000 ? "\n...(truncated)" : "") << "\n";
    }

    th.stop();
    th.join_thread();
    return success && status >= 200 && status < 300 ? 0 : 4;
}
