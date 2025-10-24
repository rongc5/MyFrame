#pragma once

#include "base_data_process.h"
#include "app_handler_v2.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class custom_stream_process : public base_data_process {
public:
    static const uint32_t PROTOCOL_ID = 1000;

    custom_stream_process(std::shared_ptr<base_net_obj> p, myframe::IApplicationHandler* app)
        : base_data_process(std::move(p)), _app_handler(app) {}

    size_t process_recv_buf(const char* buf, size_t len) override {
        if (!buf || len == 0) return 0;
        std::string* out = new std::string;
        if (_app_handler) {
            myframe::BinaryRequest req;
            req.protocol_id = PROTOCOL_ID;
            req.payload.assign(reinterpret_cast<const uint8_t*>(buf),
                               reinterpret_cast<const uint8_t*>(buf) + len);

            myframe::BinaryResponse resp;
            myframe::detail::HandlerContextScope scope(this);
            _app_handler->on_binary(req, resp);

            if (!resp.data.empty()) {
                out->assign(reinterpret_cast<const char*>(resp.data.data()), resp.data.size());
            } else {
                out->assign(buf, buf + len);
            }
        } else {
            out->assign(buf, buf + len);
        }
        put_send_buf(out);
        return len;
    }

    std::string* get_send_buf() override {
        return base_data_process::get_send_buf();
    }
    void handle_msg(std::shared_ptr<normal_msg>& msg) override {
        if (_app_handler) {
            myframe::detail::HandlerContextScope scope(this);
            _app_handler->handle_msg(msg);
        }
    }
    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (_app_handler) {
            myframe::detail::HandlerContextScope scope(this);
            _app_handler->handle_timeout(t_msg);
        }
    }

private:
    myframe::IApplicationHandler* _app_handler;
};
