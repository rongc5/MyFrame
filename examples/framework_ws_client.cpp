#include "../core/base_net_thread.h"
#include "../core/out_connect.h"
#include "../core/web_socket_req_process.h"
#include "../core/web_socket_data_process.h"
#include "../core/common_exception.h"
#include "../core/string_pool.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class FrameworkWsClientProcess : public web_socket_data_process {
public:
    FrameworkWsClientProcess(web_socket_process* process,
                             std::vector<std::string> messages)
        : web_socket_data_process(process)
        , messages_(std::move(messages)) {}

    void on_handshake_ok() override {
        std::cout << "[Client] Handshake OK" << std::endl;
        send_next();
    }

    void msg_recv_finish() override {
        std::cout << "[Client] ← " << _recent_msg << std::endl;
        received_.push_back(_recent_msg);
        _recent_msg.clear();  // Clear buffer to prevent message accumulation

        if (!send_next()) {
            std::cout << "[Client] Received " << received_.size()
                      << " messages. Test complete." << std::endl;
            base_thread::stop_all_thread();
        }
    }

    void on_close() override {
        std::cout << "[Client] Connection closed." << std::endl;
    }

private:
    bool send_next() {
        if (current_index_ >= messages_.size()) return false;
        ws_msg_type msg;
        auto* payload = myframe::string_acquire();
        payload->assign(messages_[current_index_]);
        msg._p_msg = payload;
        msg._con_type = 0x1; // text
        put_send_msg(msg);
        _process->notice_send();
        std::cout << "[Client] → " << messages_[current_index_] << std::endl;
        ++current_index_;
        return true;
    }

private:
    std::vector<std::string> messages_;
    std::vector<std::string> received_;
    size_t current_index_{0};
};

void install_signal_handlers() {
    std::signal(SIGINT, [](int) { base_thread::stop_all_thread(); });
    std::signal(SIGTERM, [](int) { base_thread::stop_all_thread(); });
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    unsigned short port = 19090;
    std::string path = "/ws";
    std::vector<std::string> payloads = {
        "ping",
        "code_all md5?",
        "framework echo verification"
    };

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<unsigned short>(std::stoi(argv[2]));
    if (argc > 3) path = argv[3];
    if (argc > 4) {
        payloads.assign(argv + 4, argv + argc); // custom test messages
    }

    std::cout << "=== MyFrame WS Framework Client ===" << std::endl;
    std::cout << "Target: ws://" << host << ":" << port << path << std::endl;

    try {
        install_signal_handlers();

        base_net_thread net_thread;
        net_thread.start();

        auto conn = std::make_shared<out_connect<web_socket_req_process>>(host, port);
        auto req = new web_socket_req_process(conn);
        req->get_req_para()._s_path = path;
        req->get_req_para()._s_host = host + ":" + std::to_string(port);
        req->get_req_para()._origin = "http://" + host + ":" + std::to_string(port);

        req->set_process(new FrameworkWsClientProcess(req, payloads));
        conn->set_process(req);
        conn->set_net_container(net_thread.get_net_container());

        std::cout << "[Client] Connecting..." << std::endl;
        conn->connect();

        std::thread watchdog([]() {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            std::cerr << "[Client] Timeout reached, stopping." << std::endl;
            base_thread::stop_all_thread();
        });
        watchdog.detach();

        net_thread.join_thread();
        std::cout << "[Client] Done." << std::endl;
        return 0;

    } catch (const CMyCommonException& ex) {
        std::cerr << "[Client] MyFrame exception: " << ex.what() << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "[Client] std::exception: " << ex.what() << std::endl;
    }
    return 1;
}
