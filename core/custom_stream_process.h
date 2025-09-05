#pragma once

#include "base_data_process.h"
#include "app_handler.h"
#include <cstdint>

class custom_stream_process : public base_data_process {
public:
    static const uint32_t PROTOCOL_ID = 1000;
    custom_stream_process(std::shared_ptr<base_net_obj> p, IAppHandler* app)
        : base_data_process(p), _app_handler(app) {}
    virtual size_t process_recv_buf(const char* buf, size_t len) override {
        if (!buf || len == 0) return 0; if (_app_handler) _app_handler->on_custom(PROTOCOL_ID, buf, len); return len; }
    virtual std::string* get_send_buf() override { return 0; }
private:
    IAppHandler* _app_handler;
};
