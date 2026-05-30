// em_xdxr_test — diagnostic for "pre-resolve hostname into IP" anti-pattern.
//
// PRINCIPLE this example documents:
//
//   HTTP has TWO concepts that look the same but are NOT:
//     - authority host    → goes into HTTP Host header and TLS SNI
//                           (the *logical* identity of the server)
//     - connect address   → the IP we open the TCP socket to
//                           (a *transport* detail, may be any IP that
//                            serves the authority)
//
//   When a caller does `resolved_ip = resolveHost(host); out_connect(resolved_ip, port)`,
//   the IP often ends up being treated as BOTH (Host header / SNI / wire authority).
//   For servers behind shared LB / VHost / SNI-multiplexed TLS, sending an IP as
//   authority is rejected (403 / handshake fail / silent close).
//
//   Correct usage: ALWAYS pass the hostname to out_connect; let the framework
//   resolve internally. If the framework wants to cache DNS, that's its concern,
//   not the caller's.
//
// What this example verifies:
//   1. Hostname mode  (correct usage)        → expected 200 OK
//   2. Pre-resolved IP mode (anti-pattern)   → expected 403 / non-2xx,
//                                              kept as a diagnostic to prevent
//                                              future callers from re-introducing
//                                              the bug.
//
// The default invocation runs case 1 against EastMoney's
// RPT_SHAREBONUS_DET endpoint (the URL that originally triggered the bug in
// stockbridge daemon 2026-05-30). The output should always be 200 + a JSON body.
//
// To reproduce the anti-pattern:
//   IP=$(getent hosts datacenter-web.eastmoney.com | awk '{print $1}' | head -1)
//   ./em_xdxr_test "http://$IP/api/data/v1/get?...same path..."
//   # → expect 403; this is the diagnostic, NOT a regression.
//
// Usage:
//   ./em_xdxr_test                       # default: hostname mode on EastMoney
//   ./em_xdxr_test <url>                 # any URL (hostname or IP authority)
//
// Background: stockbridge daemon (DictUpdateThread::httpGet) used to do
//   resolved_ip = resolveHost(host); out_connect(resolved_ip, port);
// 100% failed on this endpoint. Fix: out_connect(host, port). See StockBridge
// commit 9aeb488 for the upstream consumer fix.
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
                      std::string& scheme, std::string& authority,
                      unsigned short& port, std::string& path) {
    std::regex re(R"((http|https)://([^/ :]+)(:([0-9]+))?(/.*)?)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) return false;
    scheme = m[1].str();
    authority = m[2].str();
    port = m[4].matched ? static_cast<unsigned short>(std::stoi(m[4].str()))
                        : static_cast<unsigned short>(scheme == "https" ? 443 : 80);
    path = m[5].matched ? m[5].str() : "/";
    return true;
}

// Heuristic: authority looks like a literal IPv4 if all 4 dot-separated parts
// are pure digits. Good enough for diagnostic warning.
static bool looks_like_ipv4(const std::string& s) {
    int parts = 0, digits = 0;
    for (char c : s) {
        if (c == '.') { if (digits == 0) return false; ++parts; digits = 0; }
        else if (c >= '0' && c <= '9') ++digits;
        else return false;
    }
    return parts == 3 && digits > 0;
}

int main(int argc, char** argv) {
    // Default URL: the EastMoney query that exposed the bug originally.
    std::string url = (argc > 1) ? argv[1] :
        "http://datacenter-web.eastmoney.com/api/data/v1/get"
        "?reportName=RPT_SHAREBONUS_DET"
        "&columns=SECURITY_CODE,EX_DIVIDEND_DATE,PRETAX_BONUS_RMB,IT_RATIO,BONUS_RATIO,ASSIGN_PROGRESS"
        "&filter=%28EX_DIVIDEND_DATE%3E%3D%272026-06-01%27%29"
        "%28ASSIGN_PROGRESS%3D%22%E5%AE%9E%E6%96%BD%E5%88%86%E9%85%8D%22%29"
        "&pageNumber=1&pageSize=5"
        "&sortColumns=EX_DIVIDEND_DATE&sortTypes=-1";

    std::string scheme, authority, path;
    unsigned short port;
    if (!parse_url(url, scheme, authority, port, path)) {
        std::cerr << "[em_xdxr_test] bad URL\n";
        return 1;
    }

    const bool ip_authority = looks_like_ipv4(authority);
    std::cerr << "[em_xdxr_test] scheme=" << scheme
              << " authority=" << authority
              << " port=" << port
              << " mode=" << (ip_authority ? "IP (anti-pattern)" : "hostname (correct)")
              << "\n";
    std::cerr << "[em_xdxr_test] path_len=" << path.size() << "\n";

    if (ip_authority) {
        std::cerr << "[em_xdxr_test] NOTE: authority is a literal IP. This is the "
                  << "anti-pattern this example exists to catch. Expecting 403 or "
                  << "non-2xx from VHost-backed servers (e.g. EastMoney).\n";
    }

    base_net_thread th;
    if (!th.start()) {
        std::cerr << "[em_xdxr_test] base_net_thread start failed\n";
        return 2;
    }

    // KEY POINT: pass `authority` (whatever the URL gave us) directly. The
    // framework does its own DNS resolution + TCP connect. We never call
    // resolveHost / getaddrinfo ourselves. The Host header and TLS SNI both
    // derive from `authority`, so they always match what the URL says.
    std::shared_ptr<out_connect<http_req_process>> conn;
#ifdef ENABLE_SSL
    if (scheme == "https") {
        conn = std::make_shared<tls_out_connect<http_req_process>>(
            authority, port, authority, "http/1.1");
    } else
#endif
    {
        conn = std::make_shared<out_connect<http_req_process>>(authority, port);
    }

    auto proc = new http_req_process(conn);

    std::map<std::string, std::string> headers;
    headers["Host"] = authority;   // mirror what the URL said
    headers["User-Agent"] = "em_xdxr_test/1.0";
    headers["Connection"] = "close";
    headers["Accept-Encoding"] = "identity";
    headers["Referer"] = "https://emweb.securities.eastmoney.com/";

    auto client = new http_client_data_process(proc, "GET", authority, path, headers, "");
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

    // Expectation summary so test-runner / CI can grep for it
    if (ip_authority) {
        std::cerr << "[em_xdxr_test] verdict: " << (status == 200
            ? "UNEXPECTED 200 (server accepted IP authority — VHost may be lenient)"
            : (status == 403 || status == 404
                ? "EXPECTED non-2xx (server rejected IP authority, this is the anti-pattern proof)"
                : "INCONCLUSIVE (status not 200/403/404)")) << "\n";
    } else {
        std::cerr << "[em_xdxr_test] verdict: " << (status == 200
            ? "EXPECTED 200 (hostname authority works as intended)"
            : "REGRESSION (hostname authority should succeed — investigate framework)") << "\n";
    }

    if (!body.empty()) {
        std::cout << body.substr(0, 1000)
                  << (body.size() > 1000 ? "\n...(truncated)" : "") << "\n";
    }

    th.stop();
    th.join_thread();
    // Exit code: 0 only if the hostname-mode case succeeded. IP-mode is a
    // diagnostic, so we don't fail CI on it; that's why we don't gate on
    // ip_authority here.
    return (success && status == 200) ? 0 : 4;
}
