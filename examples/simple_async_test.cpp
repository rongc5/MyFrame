#include "server.h"
#include "unified_protocol_factory.h"
#include "http_base_data_process.h"
#include "http_res_process.h"
#include "http_base_process.h"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

class SimpleAsyncDataProcess : public http_base_data_process {
private:
    static const uint32_t TIMER_ASYNC = 1;
    static const uint32_t TIMER_TIMEOUT = 2;

    std::string recv_body_;
    std::string send_body_;
    bool async_finished_;

public:
    explicit SimpleAsyncDataProcess(http_base_process* process)
        : http_base_data_process(process)
        , async_finished_(false) {}

    size_t process_recv_body(const char* buf, size_t len, int& result) override {
        recv_body_.append(buf, len);
        result = 1;
        return len;
    }

    void msg_recv_finish() override {
        auto& req = _base_process->get_req_head_para();
        auto& res = _base_process->get_res_head_para();

        std::cout << "[HTTP] " << req._method << " " << req._url_path << std::endl;

        if (req._url_path == "/") {
            handle_home(res);
        } else if (req._url_path == "/sync") {
            handle_sync(res);
        } else if (req._url_path == "/async") {
            handle_async();
        } else {
            res._response_code = 404;
            res._response_str = "Not Found";
            send_body_ = "404 Not Found";
            res._headers["Content-Length"] = std::to_string(send_body_.size());
        }
    }

    std::string* get_send_head() override {
        if (async_response_pending()) {
            return nullptr;
        }
        auto& res = _base_process->get_res_head_para();
        auto* head = new std::string();
        res.to_head_str(head);
        return head;
    }

    std::string* get_send_body(int& result) override {
        result = 1;
        if (send_body_.empty()) {
            return nullptr;
        }
        auto* body = new std::string(send_body_);
        send_body_.clear();
        return body;
    }

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (!t_msg || async_finished_) return;

        auto& res = _base_process->get_res_head_para();
        res._headers["Content-Type"] = "application/json";

        if (t_msg->_timer_type == TIMER_ASYNC) {
            async_finished_ = true;
            res._response_code = 200;
            res._response_str = "OK";
            send_body_ = "{\"type\":\"async\",\"status\":\"ok\"}";
        } else if (t_msg->_timer_type == TIMER_TIMEOUT) {
            async_finished_ = true;
            res._response_code = 504;
            res._response_str = "Gateway Timeout";
            send_body_ = "{\"type\":\"error\",\"status\":\"timeout\"}";
        } else {
            return;
        }

        res._headers["Content-Length"] = std::to_string(send_body_.size());
        complete_async_response();
    }

private:
    void handle_home(http_res_head_para& res) {
        res._response_code = 200;
        res._response_str = "OK";
        res._headers["Content-Type"] = "text/html; charset=utf-8";
        send_body_ =
            "<!DOCTYPE html>\n"
            "<html><head><title>Async demo</title></head><body>\n"
            "<h1>Async response demo</h1>\n"
            "<ul><li><a href=\"/sync\">Sync response</a></li>\n"
            "<li><a href=\"/async\">Async response (100 ms)</a></li></ul>\n"
            "</body></html>\n";
        res._headers["Content-Length"] = std::to_string(send_body_.size());
    }

    void handle_sync(http_res_head_para& res) {
        res._response_code = 200;
        res._response_str = "OK";
        res._headers["Content-Type"] = "application/json";
        send_body_ = "{\"type\":\"sync\",\"message\":\"immediate\"}";
        res._headers["Content-Length"] = std::to_string(send_body_.size());
    }

    void handle_async() {
        set_async_response_pending(true);
        async_finished_ = false;

        auto timer = std::make_shared<timer_msg>();
        timer->_timer_type = TIMER_ASYNC;
        timer->_time_length = 100;
        if (auto conn = get_base_net()) {
            timer->_obj_id = conn->get_id()._id;
        }
        add_timer(timer);

        auto timeout_timer = std::make_shared<timer_msg>();
        timeout_timer->_timer_type = TIMER_TIMEOUT;
        timeout_timer->_time_length = 5000;
        if (auto conn = get_base_net()) {
            timeout_timer->_obj_id = conn->get_id()._id;
        }
        add_timer(timeout_timer);
    }
};

class SimpleAsyncProcess : public http_res_process {
public:
    explicit SimpleAsyncProcess(std::shared_ptr<base_net_obj> conn)
        : http_res_process(conn) {
        set_process(new SimpleAsyncDataProcess(this));
    }
};

int main(int argc, char* argv[]) {
    unsigned short port = 8081;
    if (argc > 1) {
        port = static_cast<unsigned short>(std::atoi(argv[1]));
    }

    std::cout << "Simple async HTTP example" << std::endl;
    std::cout << "Listening on 0.0.0.0:" << port << std::endl;

    try {
        ::server srv(2);
        auto factory = std::make_shared<myframe::UnifiedProtocolFactory>();

        factory->register_custom_protocol(
            "simple_async",
            [](const char* data, size_t len) {
                if (len < 3) return false;
                return std::memcmp(data, "GET", 3) == 0 || std::memcmp(data, "POS", 3) == 0;
            },
            [](std::shared_ptr<base_net_obj> conn) {
                return std::unique_ptr<base_data_process>(new SimpleAsyncProcess(conn));
            },
            15);

        srv.bind("0.0.0.0", port);
        srv.set_business_factory(factory);
        srv.start();
        srv.join();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
