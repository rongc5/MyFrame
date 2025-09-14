#pragma once

#include "base_data_process.h"
#include <map>
#include <string>

// Auto HTTPS client process: negotiates ALPN and routes to HTTP/2 or HTTP/1.1
// - If ALPN selects "h2", delegate to http2_client_process
// - Else, send a minimal HTTP/1.1 request and parse response
class hybrid_https_client_process : public base_data_process {
public:
    hybrid_https_client_process(std::shared_ptr<base_net_obj> c,
                                const std::string& method,
                                const std::string& host,
                                const std::string& path,
                                const std::map<std::string,std::string>& headers,
                                const std::string& body);
    virtual ~hybrid_https_client_process() {}

    virtual size_t process_recv_buf(const char* buf, size_t len) override;
    virtual std::string* get_send_buf() override;
    virtual void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override;

private:
    enum Mode { UNKNOWN=0, H2, HTTP1 };
    bool maybe_init(); // returns true when initialized
    size_t process_http1_recv(const char* buf, size_t len);
    void enqueue_http1_request();

private:
    // Request params
    std::string _method, _host, _path, _body;
    std::map<std::string,std::string> _headers;

    // Mode and delegation
    Mode _mode{UNKNOWN};
    std::unique_ptr<base_data_process> _inner; // for H2
    bool _sent_http1{false};

    // HTTP/1.1 parse state
    bool _h1_headers_done{false};
    int _h1_status{0};
    size_t _h1_content_length{0};
    bool _h1_chunked{false};
    std::string _h1_buf;
    std::string _h1_body;
};

