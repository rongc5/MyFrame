#include "../core/base_net_thread.h"
#include "../core/out_connect.h"
#include "../core/tls_out_connect.h"
#include "../core/http2_client_process.h"
#include "../core/base_thread.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <thread>

namespace {
struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
};

static bool parse_url(const std::string& url, ParsedUrl& out) {
    // Accept https:// or h2:// URLs
    std::regex re(R"(((?:https)|(?:h2))://([^/ :]+)(:([0-9]+))?(/.*)?)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(url, m, re)) return false;
    out.scheme = m[1];
    std::transform(out.scheme.begin(), out.scheme.end(), out.scheme.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    out.host = m[2];
    out.port = m[4].matched ? m[4].str() : std::string("443");
    out.path = m[5].matched ? m[5].str() : std::string("/");
    return true;
}

static void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <https|h2 URL> [--method VERB] [--body DATA] [--header name:value] [--timeout-ms N]\n";
    std::cerr << "Example: " << argv0 << " https://127.0.0.1:7779/hello --header accept:application/json\n";
}
}

#ifndef ENABLE_SSL
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::cerr << "http2_out_client requires TLS support. Rebuild with ENABLE_SSL enabled." << std::endl;
    return 2;
}
#else
int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    ParsedUrl parsed;
    if (!parse_url(argv[1], parsed)) {
        std::cerr << "Bad URL: " << argv[1] << std::endl;
        return 2;
    }

    std::string method("GET");
    std::string body;
    std::map<std::string,std::string> headers;
    int timeout_ms = 15000;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--method" || arg == "-X") && i + 1 < argc) {
            method = argv[++i];
        } else if ((arg == "--body" || arg == "--data" || arg == "-d") && i + 1 < argc) {
            body = argv[++i];
        } else if (arg == "--header" && i + 1 < argc) {
            std::string kv = argv[++i];
            auto pos = kv.find(':');
            if (pos == std::string::npos || pos == 0 || pos + 1 >= kv.size()) {
                std::cerr << "--header expects name:value" << std::endl;
                return 3;
            }
            std::string name = kv.substr(0, pos);
            std::string value = kv.substr(pos + 1);
            // Trim optional leading space from value
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
            headers[name] = value;
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = std::atoi(argv[++i]);
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    bool tls = (parsed.scheme == "https" || parsed.scheme == "h2");
    if (!tls) {
        std::cerr << "Only https:// and h2:// URLs are supported." << std::endl;
        return 2;
    }

    unsigned short port = (unsigned short)std::atoi(parsed.port.c_str());
    if (port == 0) port = 443;

    std::string authority = parsed.host;
    if (!parsed.port.empty() && parsed.port != "443") {
        authority.assign(parsed.host).append(":").append(parsed.port);
    }

    base_net_thread net_thread;
    net_thread.start();

    std::shared_ptr< out_connect<http2_client_process> > conn;
    conn = std::make_shared< tls_out_connect<http2_client_process> >(parsed.host, port, parsed.host, std::string("h2"));

    auto proc = new http2_client_process(conn,
                                         method,
                                         std::string("https"),
                                         authority,
                                         parsed.path,
                                         headers,
                                         body);
    conn->set_process(proc);
    conn->set_net_container(net_thread.get_net_container());
    std::shared_ptr<base_net_obj> net = conn;
    net_thread.get_net_container()->push_real_net(net);

    conn->connect();

    std::thread waiter([proc, timeout_ms] {
        bool ok = proc->wait_done(timeout_ms);
        if (ok) {
            std::cout << "HTTP/2 status: " << proc->status() << "\n";
            std::cout << proc->response_body() << std::endl;
        } else {
            std::cerr << "Timed out waiting for HTTP/2 response" << std::endl;
            proc->request_close_now();
        }
        base_thread::stop_all_thread();
    });

    net_thread.join_thread();
    waiter.join();
    return 0;
}
#endif
