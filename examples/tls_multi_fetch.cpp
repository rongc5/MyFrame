#include "../core/base_net_thread.h"
#include "../core/http_client_data_process.h"
#include "../core/http_req_process.h"
#include "../core/out_connect.h"
#include "../core/tls_out_connect.h"
#include "../core/ssl_context.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string path;
    unsigned short port{0};
};

bool parse_url(const std::string& url, ParsedUrl& out) {
    static const std::regex kUrlRegex(R"((http|https)://([^/:]+)(:([0-9]+))?(/.*)?)",
                                      std::regex::icase);
    std::smatch m;
    if (!std::regex_match(url, m, kUrlRegex)) {
        return false;
    }
    out.scheme = m[1].str();
    std::transform(out.scheme.begin(), out.scheme.end(), out.scheme.begin(), ::tolower);
    out.host = m[2].str();
    out.port = m[4].matched ? static_cast<unsigned short>(std::stoi(m[4].str()))
                            : static_cast<unsigned short>(out.scheme == "https" ? 443 : 80);
    out.path = m[5].matched ? m[5].str() : "/";
    if (out.path.empty()) {
        out.path = "/";
    }
    return true;
}

void configure_tls(const std::string& ca_file) {
    ssl_config conf;
    conf._ca_file = ca_file;
    conf._verify_peer = !ca_file.empty();
    conf._enable_session_cache = true;
    conf._enable_tickets = true;
    tls_set_client_config(conf);
}

std::string load_default_ca() {
    // Try repo-local CA bundle first
    const std::vector<std::string> candidates = {
        "./data/ca-bundle.crt",
        "../data/ca-bundle.crt",
        "/etc/ssl/certs/ca-certificates.crt"
    };

    for (const auto& path : candidates) {
        std::ifstream in(path);
        if (in.good()) {
            return path;
        }
    }
    return std::string();
}

bool fetch_once(const std::string& url,
                int timeout_ms,
                const std::map<std::string, std::string>& default_headers,
                std::string& body_out,
                int& status_out) {
    ParsedUrl parsed;
    if (!parse_url(url, parsed)) {
        std::cerr << "[tls_multi_fetch] Invalid URL: " << url << std::endl;
        return false;
    }

    base_net_thread net_thread;
    if (!net_thread.start()) {
        std::cerr << "[tls_multi_fetch] Failed to start base_net_thread" << std::endl;
        return false;
    }

    std::shared_ptr< out_connect<http_req_process> > conn;
    try {
        if (parsed.scheme == "https") {
            conn = std::make_shared< tls_out_connect<http_req_process> >(
                parsed.host, parsed.port, parsed.host, "http/1.1");
        } else {
            conn = std::make_shared< out_connect<http_req_process> >(parsed.host, parsed.port);
        }
    } catch (const std::exception& ex) {
        std::cerr << "[tls_multi_fetch] Failed to create connector for " << url
                  << " error=" << ex.what() << std::endl;
        net_thread.stop();
        net_thread.join_thread();
        return false;
    }

    auto req_process = new http_req_process(conn);
    std::map<std::string, std::string> headers = default_headers;
    headers["Host"] = parsed.host;

    auto client = new http_client_data_process(req_process,
                                               "GET",
                                               parsed.host,
                                               parsed.path,
                                               headers,
                                               std::string());
    req_process->set_process(client);
    conn->set_process(req_process);
    conn->set_net_container(net_thread.get_net_container());
    std::shared_ptr<base_net_obj> base_net = conn;
    net_thread.get_net_container()->push_real_net(base_net);

    if (timeout_ms > 0) {
        auto timer = std::make_shared<timer_msg>();
        timer->_obj_id = conn->get_id()._id;
        timer->_timer_type = 0x544c5346; // "TLSF"
        timer->_time_length = static_cast<uint32_t>(timeout_ms);
        client->add_timer(timer);
    }

    bool success = false;
    try {
        conn->connect();
        const int wait_ms = timeout_ms > 0 ? timeout_ms : 15000;
        success = client->wait_done(wait_ms);
    } catch (const std::exception& ex) {
        std::cerr << "[tls_multi_fetch] Connection error for " << url
                  << " error=" << ex.what() << std::endl;
    }

    status_out = client->status();
    if (success) {
        body_out = client->response_body();
    }

    net_thread.stop();
    net_thread.join_thread();
    return success;
}

void print_summary(const std::string& url, int status, const std::string& body) {
    constexpr size_t kPreviewLen = 256;
    std::cout << "=== " << url << " ===" << std::endl;
    std::cout << "HTTP status: " << status << std::endl;
    if (body.empty()) {
        std::cout << "(empty body)" << std::endl;
        return;
    }
    std::cout << "Body preview (" << std::min(body.size(), kPreviewLen)
              << " bytes):" << std::endl;
    std::cout << body.substr(0, kPreviewLen);
    if (body.size() > kPreviewLen) {
        std::cout << "...";
    }
    std::cout << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> urls;
    std::string ca_file;
    int timeout_ms = 10000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ca" && i + 1 < argc) {
            ca_file = argv[++i];
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = std::atoi(argv[++i]);
        } else {
            urls.push_back(arg);
        }
    }

    if (urls.empty()) {
        urls = {
            "https://www.example.com/",
            "https://api.github.com/",
            "https://jsonplaceholder.typicode.com/todos/1"
        };
        std::cout << "[tls_multi_fetch] No URLs provided, using defaults:" << std::endl;
        for (const auto& url : urls) {
            std::cout << "  - " << url << std::endl;
        }
    }

    if (ca_file.empty()) {
        ca_file = load_default_ca();
    }
    if (ca_file.empty()) {
        std::cerr << "[tls_multi_fetch] WARNING: CA bundle not found, TLS verification disabled"
                  << std::endl;
    } else {
        std::cout << "[tls_multi_fetch] Using CA bundle: " << ca_file << std::endl;
    }
    configure_tls(ca_file);

    std::map<std::string, std::string> headers = {
        {"Accept", "*/*"},
        {"User-Agent", "StockBridge-TLS-MultiFetch/1.0"}
    };

    bool all_success = true;
    for (const auto& url : urls) {
        std::string body;
        int status = 0;
        bool ok = fetch_once(url, timeout_ms, headers, body, status);
        if (!ok) {
            std::cerr << "[tls_multi_fetch] Request failed for " << url << std::endl;
            all_success = false;
            continue;
        }
        print_summary(url, status, body);
    }

    if (!all_success) {
        return 1;
    }
    return 0;
}
