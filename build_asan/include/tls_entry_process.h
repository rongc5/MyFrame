#pragma once

#include "base_data_process.h"
#include "app_handler_v2.h"
#include "ssl_context.h"

class tls_entry_process : public base_data_process {
public:
    tls_entry_process(std::shared_ptr<base_net_obj> conn, myframe::IApplicationHandler* app)
        : base_data_process(conn), _app_handler(app), _installed(false) {}
    virtual size_t process_recv_buf(const char* buf, size_t len) override;
    virtual std::string* get_send_buf() override { return 0; }
    // legacy base has no peek preference hook
private:
    bool ensure_ssl_installed();
    bool init_server_ssl(SSL*& out_ssl);
    myframe::IApplicationHandler* _app_handler; bool _installed;
};
