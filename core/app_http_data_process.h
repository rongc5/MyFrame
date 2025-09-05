#pragma once

#include "http_base_data_process.h"
#include "app_handler.h"

class app_http_data_process : public http_base_data_process {
public:
    app_http_data_process(http_base_process* p, IAppHandler* h)
        : http_base_data_process(p), _handler(h), _head_ready(false), _body_ready(false) {}

    virtual void header_recv_finish() override {}
    virtual size_t process_recv_body(const char* buf, size_t len, int& result) override {
        // 仅累积，由 http_res_process 判定 Content-Length 并设置 result
        if (buf && len) _body.append(buf, len);
        result = 0;
        return len;
    }
    virtual void msg_recv_finish() override {
        // 组装 HttpRequest -> 调用 handler -> 生成响应缓存
        HttpRequest req;
        auto& rh = _base_process->get_req_head_para();
        req.method = rh._method;
        req.url = rh._url_path;
        req.version = rh._version;
        for (auto it = rh._headers.begin(); it != rh._headers.end(); ++it) {
            req.headers[it->first] = it->second;
        }
        req.body.swap(_body);

        HttpResponse rsp;
        if (_handler) {
            _handler->on_http(req, rsp);
        } else {
            rsp.status = 200; rsp.reason = "OK";
            rsp.set_header("Content-Type", "text/plain");
            rsp.body = "OK";
        }

        // 生成发送头
        if (rsp.headers.find("Content-Length") == rsp.headers.end()) {
            rsp.headers["Content-Length"] = std::to_string(rsp.body.size());
        }
        if (rsp.headers.find("Connection") == rsp.headers.end()) {
            rsp.headers["Connection"] = "close";
        }

        _p_head.reset(new std::string);
        *_p_head += "HTTP/1.1 ";
        *_p_head += std::to_string(rsp.status);
        *_p_head += " ";
        *_p_head += rsp.reason;
        *_p_head += "\r\n";
        for (auto it = rsp.headers.begin(); it != rsp.headers.end(); ++it) {
            *_p_head += it->first; *_p_head += ": "; *_p_head += it->second; *_p_head += "\r\n";
        }
        *_p_head += "\r\n";
        _head_ready = true;

        _p_body.reset(new std::string); _p_body->swap(rsp.body);
        _body_ready = true;
    }
    virtual std::string* get_send_head() override {
        if (!_head_ready || !_p_head) return 0;
        _head_ready = false; return _p_head.release();
    }
    virtual std::string* get_send_body(int& result) override {
        if (!_body_ready || !_p_body) { result = 1; return 0; }
        _body_ready = false; result = 1; return _p_body.release();
    }

private:
    IAppHandler* _handler;
    std::string _body;
    std::unique_ptr<std::string> _p_head;
    std::unique_ptr<std::string> _p_body;
    bool _head_ready;
    bool _body_ready;
};

