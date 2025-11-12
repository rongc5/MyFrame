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

namespace {

constexpr int DELAYED_SEND_TIMER = 0x9010;

class FrameworkWsIdleClientProcess : public web_socket_data_process {
public:
    FrameworkWsIdleClientProcess(web_socket_process* process,
                                 std::string payload,
                                 uint32_t delay_ms)
        : web_socket_data_process(process)
        , payload_(std::move(payload))
        , delay_ms_(delay_ms) {}

    void on_handshake_ok() override {
        std::cout << "[IdleClient] Handshake OK. Waiting " << delay_ms_ / 1000.0
                  << " seconds before sending data..." << std::endl;
        schedule_delayed_send();
    }

    void msg_recv_finish() override {
        std::cout << "[IdleClient] ← " << _recent_msg << std::endl;
        _recent_msg.clear();
        base_thread::stop_all_thread();
    }

    void on_close() override {
        std::cout << "[IdleClient] Connection closed." << std::endl;
    }

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (!t_msg || t_msg->_timer_type != DELAYED_SEND_TIMER || sent_) {
            web_socket_data_process::handle_timeout(t_msg);
            return;
        }

        send_delayed_message();
    }

private:
    void schedule_delayed_send() {
        auto timer = std::make_shared<timer_msg>();
        if (auto conn = get_base_net()) {
            timer->_obj_id = conn->get_id()._id;
        }
        timer->_timer_type = DELAYED_SEND_TIMER;
        timer->_time_length = delay_ms_;
        add_timer(timer);
    }

    void send_delayed_message() {
        std::cout << "[IdleClient] → sending payload after " << delay_ms_ / 1000.0
                  << " seconds of idle time" << std::endl;
        ws_msg_type msg;
        msg.init();
        msg._p_msg = myframe::string_acquire();
        msg._p_msg->assign(payload_);
        msg._con_type = 0x1; // TEXT
        put_send_msg(msg);
        _process->notice_send();
        sent_ = true;
    }

    std::string payload_;
    uint32_t delay_ms_;
    bool sent_{false};
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
    uint32_t delay_ms = 12000; // default: 12 seconds idle before payload
    std::string payload = "idle-check payload";

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<unsigned short>(std::stoi(argv[2]));
    if (argc > 3) path = argv[3];
    if (argc > 4) {
        uint32_t delay_s = static_cast<uint32_t>(std::stoul(argv[4]));
        delay_ms = delay_s * 1000;
    }
    if (argc > 5) {
        payload = argv[5];
    }

    std::cout << "=== MyFrame WS Idle Client ===" << std::endl;
    std::cout << "Target: ws://" << host << ":" << port << path << std::endl;
    std::cout << "Idle window: " << delay_ms / 1000.0 << " seconds" << std::endl;
    std::cout << "Payload: " << payload << std::endl;

    try {
        install_signal_handlers();

        base_net_thread net_thread;
        net_thread.start();

        auto conn = std::make_shared<out_connect<web_socket_req_process>>(host, port);
        auto req = new web_socket_req_process(conn);
        req->get_req_para()._s_path = path;
        req->get_req_para()._s_host = host + ":" + std::to_string(port);
        req->get_req_para()._origin = "http://" + host + ":" + std::to_string(port);

        req->set_process(new FrameworkWsIdleClientProcess(req, payload, delay_ms));
        conn->set_process(req);
        conn->set_net_container(net_thread.get_net_container());

        std::cout << "[IdleClient] Connecting..." << std::endl;
        conn->connect();

        // Safety watchdog: exit after 60 seconds so CI doesn't hang.
        std::thread watchdog([]() {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            std::cerr << "[IdleClient] Watchdog timeout. Stopping threads." << std::endl;
            base_thread::stop_all_thread();
        });
        watchdog.detach();

        net_thread.join_thread();
        std::cout << "[IdleClient] Done." << std::endl;
        return 0;

    } catch (const CMyCommonException& ex) {
        std::cerr << "[IdleClient] MyFrame exception: " << ex.what() << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "[IdleClient] std::exception: " << ex.what() << std::endl;
    }
    return 1;
}
