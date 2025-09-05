#pragma once

#include "http_base_data_process.h"
#include <map>

class http_req_process;

class http_client_data_process : public http_base_data_process {
public:
    http_client_data_process(http_req_process* p,
                             const std::string& method,
                             const std::string& host,
                             const std::string& path,
                             const std::map<std::string,std::string>& headers,
                             const std::string& body);
    virtual ~http_client_data_process() {}

    virtual std::string* get_send_head() override;
    virtual std::string* get_send_body(int& result) override;
    virtual void msg_recv_finish() override;
    virtual size_t process_recv_body(const char* buf, size_t len, int& result) override;

    int status() const { return _status; }
    const std::string& response_body() const { return _resp_body; }

private:
    std::string _method, _host, _path;
    std::map<std::string,std::string> _headers;
    std::string _body;
    std::unique_ptr<std::string> _p_head;
    std::unique_ptr<std::string> _p_body;
    std::string _resp_body;
    int _status{0};
};
