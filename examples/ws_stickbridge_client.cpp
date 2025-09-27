#include "../core/base_net_thread.h"
#include "../core/out_connect.h"
#include "../core/web_socket_req_process.h"
#include "../core/web_socket_data_process.h"
#include "../core/common_exception.h"
#include "../core/tls_out_connect.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string url;
    int reconnect_interval{5};
    bool print_frames{true};
    bool once{false};
};

bool parse_url(const std::string& url, std::string& scheme, std::string& host, unsigned short& port, std::string& path) {
    const std::string prefix_ws = "ws://";
    const std::string prefix_wss = "wss://";
    if (url.compare(0, prefix_ws.size(), prefix_ws) == 0) {
        scheme = "ws";
        host = url.substr(prefix_ws.size());
        port = 80;
    } else if (url.compare(0, prefix_wss.size(), prefix_wss) == 0) {
        scheme = "wss";
        host = url.substr(prefix_wss.size());
        port = 443;
    } else {
        return false;
    }

    auto slash = host.find('/');
    path = slash == std::string::npos ? "/" : host.substr(slash);
    if (slash != std::string::npos) host.erase(slash);

    auto colon = host.find(':');
    if (colon != std::string::npos) {
        port = static_cast<unsigned short>(std::stoi(host.substr(colon + 1)));
        host.erase(colon);
    }
    return !host.empty();
}

Options parse_args(int argc, char** argv) {
    Options opt;
    if (argc < 2) {
        throw std::runtime_error("Usage: ws_stickbridge_client <ws://host:port/path> [--interval N] [--silent] [--once]");
    }
    opt.url = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--interval" && i + 1 < argc) {
            opt.reconnect_interval = std::stoi(argv[++i]);
            if (opt.reconnect_interval <= 0) opt.reconnect_interval = 1;
        } else if (arg == "--silent") {
            opt.print_frames = false;
        } else if (arg == "--once") {
            opt.once = true;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    return opt;
}

class StickBridgeWsProcess : public web_socket_data_process {
public:
    StickBridgeWsProcess(web_socket_process* p, std::function<void(const std::string&)> cb, bool print_frames)
        : web_socket_data_process(p), callback_(std::move(cb)), print_frames_(print_frames) {}

    void on_handshake_ok() override {
        if (print_frames_) {
            std::cout << "[ws] handshake ok" << std::endl;
        }
    }

    void msg_recv_finish() override {
        std::string payload;
        payload.swap(_recent_msg);
        if (print_frames_ && payload.size() < 2048) {
            std::cout << "[ws] recv: " << payload << std::endl;
        }
        if (callback_) callback_(payload);
    }

    void on_close() override {
        if (print_frames_) {
            std::cout << "[ws] connection closed" << std::endl;
        }
    }

private:
    std::function<void(const std::string&)> callback_;
    bool print_frames_{true};
};

class StickBridgeClientApp {
public:
    StickBridgeClientApp(const Options& opt)
        : options_(opt) {
        std::string scheme, host, path;
        unsigned short port = 0;
        if (!parse_url(options_.url, scheme, host, port, path)) {
            throw std::runtime_error("Invalid StickBridge URL: " + options_.url);
        }
        use_tls_ = (scheme == "wss");
        host_ = std::move(host);
        port_ = port;
        path_ = std::move(path);

        thread_.reset(new base_net_thread());
        thread_->start();
    }

    ~StickBridgeClientApp() {
        stop();
    }

    void run() {
        stopping_ = false;
        install_signal_handler();
        loop();
        if (thread_) {
            thread_->join_thread();
        }
    }

    void stop() {
        stopping_ = true;
        base_thread::stop_all_thread();
    }

private:
    void install_signal_handler() {
        std::signal(SIGINT, [](int) { base_thread::stop_all_thread(); });
        std::signal(SIGTERM, [](int) { base_thread::stop_all_thread(); });
    }

    template <typename ConnType, typename... Args>
    std::shared_ptr<ConnType> make_connection(Args&&... extra) {
        auto conn = std::make_shared<ConnType>(host_, port_, std::forward<Args>(extra)...);
        auto proc = new web_socket_req_process(conn);
        auto callback = [this](const std::string& payload) { on_message(payload); };
        proc->set_process(new StickBridgeWsProcess(proc, callback, options_.print_frames));
        auto& para = proc->get_req_para();
        para._s_path = path_;
        para._s_host = host_;
        para._origin = use_tls_ ? ("https://" + host_) : ("http://" + host_);
        conn->set_process(proc);
        conn->set_net_container(thread_->get_net_container());
        return conn;
    }

    bool dial_once() {
        try {
#ifdef ENABLE_SSL
            if (use_tls_) {
                auto conn_tls = make_connection<tls_out_connect<web_socket_req_process>>(host_);
                current_conn_ = conn_tls;
                conn_tls->connect();
            } else
#endif
            {
                auto conn_plain = make_connection<out_connect<web_socket_req_process>>();
                current_conn_ = conn_plain;
                conn_plain->connect();
            }
            waiting_done_ = false;
            return true;
        } catch (const CMyCommonException& ex) {
            std::cerr << "[ws] connect failed: " << ex.what() << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "[ws] connect failed: " << ex.what() << std::endl;
        }
        return false;
    }

    void loop() {
        while (!stopping_) {
            if (dial_once()) {
                wait_for_stop_or_disconnect();
                if (options_.once) break;
            }
            if (stopping_) break;
            std::this_thread::sleep_for(std::chrono::seconds(options_.reconnect_interval));
        }
    }

    void wait_for_stop_or_disconnect() {
        auto last_seen = std::chrono::steady_clock::now();
        while (!stopping_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (waiting_done_) break;
            auto now = std::chrono::steady_clock::now();
            if (now - last_seen > std::chrono::seconds(1)) {
                last_seen = now;
            }
        }
    }

    void on_message(const std::string& payload) {
        if (payload == "close" || payload == "exit") {
            waiting_done_ = true;
            stopping_ = true;
            base_thread::stop_all_thread();
        }
    }

    Options options_;
    bool use_tls_{false};
    std::string host_;
    unsigned short port_{0};
    std::string path_;

    std::unique_ptr<base_net_thread> thread_;
    std::shared_ptr<base_net_obj> current_conn_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> waiting_done_{false};
};

} // namespace

int main(int argc, char** argv) {
    try {
        auto opt = parse_args(argc, argv);
        StickBridgeClientApp app(opt);
        app.run();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ws_stickbridge_client: " << ex.what() << std::endl;
        return 1;
    }
}
