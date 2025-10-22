// MyFrame Unified Protocol Architecture - HTTP Level 1 Adapter Implementation
#include "http_application_adapter.h"
#include "../http_base_process.h"
#include "../common_def.h"
#include "../string_pool.h"

namespace myframe {

// ============================================================================
// HttpApplicationAdapter Implementation
// ============================================================================

HttpApplicationAdapter::HttpApplicationAdapter(
    std::shared_ptr<base_net_obj> conn,
    IApplicationHandler* handler)
    : http_res_process(conn)
    , _handler(handler)
{
    // 创建并设置数据处理层
    set_process(new HttpApplicationDataProcess(this, handler));

    PDEBUG("[HttpApplicationAdapter] Created");
}

HttpApplicationAdapter::~HttpApplicationAdapter() {
    PDEBUG("[HttpApplicationAdapter] Destroyed");
}

std::unique_ptr<base_data_process> HttpApplicationAdapter::create(
    std::shared_ptr<base_net_obj> conn,
    IApplicationHandler* handler)
{
    return std::unique_ptr<base_data_process>(
        new HttpApplicationAdapter(conn, handler));
}

// ============================================================================
// HttpApplicationDataProcess Implementation
// ============================================================================

HttpApplicationDataProcess::HttpApplicationDataProcess(
    http_base_process* process,
    IApplicationHandler* handler)
    : http_base_data_process(process)
    , _handler(handler)
    , _body_sent(false)
{
    PDEBUG("[HttpApplicationDataProcess] Created");
}

HttpApplicationDataProcess::~HttpApplicationDataProcess() {
    PDEBUG("[HttpApplicationDataProcess] Destroyed");
}

void HttpApplicationDataProcess::msg_recv_finish() {
    PDEBUG("[HttpApplicationDataProcess] msg_recv_finish called");

    // 构造 HttpRequest
    HttpRequest req;

    // 获取请求头信息
    auto& req_head = _base_process->get_req_head_para();
    req.method = req_head._method;
    req.url = req_head._url_path;
    req.version = req_head._version;
    req.headers = req_head._headers;

    // 获取请求体（暂时使用空字符串，后续可扩展）
    req.body = "";

    PDEBUG("[HttpApplicationDataProcess] Request: %s %s",
           req.method.c_str(), req.url.c_str());

    // 构造 HttpResponse
    HttpResponse res;

    try {
        // 调用用户处理器
        _handler->on_http(req, res);

        PDEBUG("[HttpApplicationDataProcess] Response: %d %s (body size=%zu)",
               res.status, res.reason.c_str(), res.body.size());

        // 保存响应体
        _response_body = res.body;
        _body_sent = false;

        // 设置响应头参数
        auto& res_head = _base_process->get_res_head_para();
        res_head._response_code = res.status;
        res_head._response_str = res.reason;
        res_head._headers = res.headers;

        // 自动设置 Content-Length
        if (res_head._headers.find("Content-Length") == res_head._headers.end()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%zu", _response_body.size());
            res_head._headers["Content-Length"] = buf;
        }

        // 默认设置 Connection: close（简单实现）
        if (res_head._headers.find("Connection") == res_head._headers.end()) {
            res_head._headers["Connection"] = "close";
        }

        // 触发发送流程
        _base_process->change_http_status(SEND_HEAD);

    } catch (const std::exception& e) {
        PDEBUG("[HttpApplicationDataProcess] Exception: %s", e.what());

        // 发生异常，返回500错误
        _response_body = "Internal Server Error";
        _body_sent = false;

        auto& res_head = _base_process->get_res_head_para();
        res_head._response_code = 500;
        res_head._response_str = "Internal Server Error";
        res_head._headers.clear();
        res_head._headers["Content-Type"] = "text/plain";
        res_head._headers["Content-Length"] = "21"; // "Internal Server Error" length
        res_head._headers["Connection"] = "close";

        _base_process->change_http_status(SEND_HEAD);
    }
}

std::string* HttpApplicationDataProcess::get_send_head() {
    PDEBUG("[HttpApplicationDataProcess] get_send_head called");

    auto& res_head = _base_process->get_res_head_para();

    // 从字符串池分配
    std::string* send_head = myframe::string_acquire();

    // 状态行: HTTP/1.1 200 OK\r\n
    *send_head += "HTTP/1.1 ";
    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", res_head._response_code);
    *send_head += status_buf;
    *send_head += " ";
    *send_head += res_head._response_str;
    *send_head += "\r\n";

    // 头部字段
    for (const auto& kv : res_head._headers) {
        *send_head += kv.first;
        *send_head += ": ";
        *send_head += kv.second;
        *send_head += "\r\n";
    }

    // 空行
    *send_head += "\r\n";

    PDEBUG("[HttpApplicationDataProcess] Response header size=%zu", send_head->size());

    return send_head;
}

std::string* HttpApplicationDataProcess::get_send_body(int& result) {
    PDEBUG("[HttpApplicationDataProcess] get_send_body called, body_sent=%d", _body_sent);

    if (_body_sent) {
        result = 1;  // 结束发送
        return nullptr;
    }

    _body_sent = true;

    if (_response_body.empty()) {
        result = 1;  // 没有 body，结束发送
        return nullptr;
    }

    result = 0;  // 正常返回，有body

    // 从字符串池分配并复制body内容
    std::string* send_body = myframe::string_acquire();
    *send_body = _response_body;

    return send_body;
}

} // namespace myframe
