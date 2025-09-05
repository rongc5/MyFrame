#include "http_client_data_process.h"
#include "http_req_process.h"
#include "http_base_process.h"
#include <sstream>

http_client_data_process::http_client_data_process(http_req_process* p,
                                                   const std::string& method,
                                                   const std::string& host,
                                                   const std::string& path,
                                                   const std::map<std::string,std::string>& headers,
                                                   const std::string& body)
    : http_base_data_process((http_base_process*)p)
    , _method(method), _host(host), _path(path), _headers(headers), _body(body)
{
}

std::string* http_client_data_process::get_send_head() {
    if (_p_head) return nullptr; // already built
    _p_head.reset(new std::string);
    std::stringstream ss;
    ss << _method << " " << (_path.empty()?"/":_path) << " HTTP/1.1\r\n";
    if (_headers.find("Host")==_headers.end()) ss << "Host: " << _host << "\r\n";
    if (_headers.find("Connection")==_headers.end()) ss << "Connection: close\r\n";
    for (auto& kv : _headers) ss << kv.first << ": " << kv.second << "\r\n";
    if (!_body.empty()) ss << "Content-Length: " << _body.size() << "\r\n";
    ss << "\r\n";
    *_p_head = ss.str();
    return _p_head.release();
}

std::string* http_client_data_process::get_send_body(int& result) {
    if (_body.empty() || _p_body) { result = 1; return nullptr; }
    _p_body.reset(new std::string(_body));
    result = 1; return _p_body.release();
}

size_t http_client_data_process::process_recv_body(const char* buf, size_t len, int& result) {
    if (buf && len) _resp_body.append(buf, len);
    result = 0; return len;
}

void http_client_data_process::msg_recv_finish() {
    // read status and headers from response head
    auto& hp = _base_process->get_res_head_para();
    _status = (int)hp._response_code;
}
