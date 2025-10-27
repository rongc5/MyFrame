// MyFrame Unified Protocol Architecture - HTTP Context Adapter Implementation
#include "http_context_adapter.h"
#include "../base_net_obj.h"
#include "../string_pool.h"
#include "../common_obj_container.h"
#include "../base_net_thread.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <thread>

namespace myframe {

// ============================================================================
// HttpContextImpl Implementation
// ============================================================================

HttpContextImpl::HttpContextImpl(http_base_process* process, std::shared_ptr<base_net_obj> conn)
    : _process(process), _conn(conn), _streaming_enabled(false) {
    // ��ʼ��������Ϣ
    if (conn) {
        // ע��base_net_obj ��ǰ���ṩ IP/�˿ڷ��ʽӿ�
        _conn_info.remote_ip = "";
        _conn_info.remote_port = 0;
        _conn_info.local_ip = "";
        _conn_info.local_port = 0;
        _conn_info.connection_id = conn->get_id()._id;
    }
}

HttpContextImpl::~HttpContextImpl() {
    // �����û�����
    _user_data.clear();
}

const HttpRequest& HttpContextImpl::request() const {
    return _request;
}

HttpResponse& HttpContextImpl::response() {
    return _response;
}

void HttpContextImpl::async_response(std::function<void()> fn) {
    // 设置异步响应标志，阻止立即发送
    if (_process) {
        auto* data_process = _process->get_process();
        if (data_process) {
            auto* http_data_process = dynamic_cast<http_base_data_process*>(data_process);
            if (http_data_process) {
                http_data_process->set_async_response_pending(true);
                PDEBUG("[HttpContext] Async response mode enabled");
            }
        }
    }

    // 启动异步执行，在后台线程执行回调
    if (fn) {
        // 注意：这里需要配合业务线程或使用线程池
        // 当前实现：直接在新线程中执行
        // TODO: 可以改用线程池或业务线程调度
        std::thread([fn, this]() {
            fn();
            // 回调执行完成后，需要调用 complete_async_response() 来触发发送
        }).detach();
    }
}

void HttpContextImpl::complete_async_response() {
    // ����첽��Ӧ��ɲ�֪ͨ���Է���
    // ���ƾɿ�ܵ� set_async_response_pending(false) + notify_send_ready()
    if (_process) {
        // ��Ҫͨ�� http_base_data_process ����
        auto* data_process = _process->get_process();
        if (data_process) {
            // ���� http_base_data_process �� complete_async_response()
            auto* http_data_process = dynamic_cast<http_base_data_process*>(data_process);
            if (http_data_process) {
                http_data_process->complete_async_response();
            }
        }
    }
}

void HttpContextImpl::enable_streaming() {
    _streaming_enabled = true;
}

size_t HttpContextImpl::stream_write(const void* data, size_t len) {
    if (!_streaming_enabled || !_conn) return 0;

    // ����ʽģʽ�£�ֱ��д������
    if (data && len > 0) {
        auto buf = string_acquire();
        if (buf) {
            buf->assign(static_cast<const char*>(data), len);
            // TODO: ʵ���첽����
            // _conn->async_send(buf);
            string_release(buf);
        }
    }
    return len;
}

void HttpContextImpl::upgrade_to_websocket() {
    // WebSocket ������Ҫ�� HTTP ������ʵ��
    // ����ֻ�Ǳ�ǣ�ʵ������������������
}

void HttpContextImpl::close() {
    if (_conn) {
        // ע��base_net_obj û�� close() ����
        // ʹ�� request_close_now() ͨ�� base_data_process
        // TODO: ʵ����ȷ�Ĺر��߼�
    }
}

void HttpContextImpl::set_timeout(uint64_t ms) {
    // �������ӳ�ʱ
    (void)ms;
}

void HttpContextImpl::keep_alive(bool enable) {
    if (enable) {
        _response.set_header("Connection", "keep-alive");
    } else {
        _response.set_header("Connection", "close");
    }
}

void HttpContextImpl::set_user_data(const std::string& key, void* data) {
    _user_data[key] = data;
}

void* HttpContextImpl::get_user_data(const std::string& key) const {
    auto it = _user_data.find(key);
    return it != _user_data.end() ? it->second : nullptr;
}

ConnectionInfo& HttpContextImpl::connection_info() {
    return _conn_info;
}

std::shared_ptr<base_net_obj> HttpContextImpl::raw_connection() {
    return _conn;
}

base_net_thread* HttpContextImpl::get_thread() const {
    if (_conn) {
        auto* container = _conn->get_net_container();
        if (container) {
            return ::base_net_thread::get_base_net_thread_obj(container->get_thread_index());
        }
    }
    return nullptr;
}

// ============================================================================
// HttpContextAdapter Implementation
// ============================================================================

HttpContextAdapter::HttpContextAdapter(
    std::shared_ptr<base_net_obj> conn,
    IProtocolHandler* handler)
    : http_res_process(conn), _handler(handler) {
    // �������������ݴ�����
    set_process(new HttpContextDataProcess(this, handler));
}

HttpContextAdapter::~HttpContextAdapter() {
}

// ��̬��������
std::unique_ptr<::base_data_process> HttpContextAdapter::create(
    std::shared_ptr<base_net_obj> conn,
    IProtocolHandler* handler) {
    return std::unique_ptr<::base_data_process>(new HttpContextAdapter(conn, handler));
}

// ============================================================================
// HttpContextDataProcess Implementation
// ============================================================================

HttpContextDataProcess::HttpContextDataProcess(
    http_base_process* process,
    IProtocolHandler* handler)
    : http_base_data_process(process), _handler(handler) {
    // ���������Ķ���
    _context = std::make_shared<HttpContextImpl>(process, process ? process->get_base_net() : nullptr);
}

HttpContextDataProcess::~HttpContextDataProcess() {
}

size_t HttpContextDataProcess::process_recv_body(const char* buf, size_t len, int& result) {
    // ���ղ��洢������
    _recv_body.append(buf, len);
    result = 1;
    return len;
}

std::string* HttpContextDataProcess::get_send_head() {
    if (async_response_pending()) {
        // �첽��Ӧ�ȴ���
        return NULL;
    }
    
    // ������Ӧͷ
    http_res_head_para& res = _base_process->get_res_head_para();
    std::string* head = new std::string();
    res.to_head_str(head);
    return head;
}

std::string* HttpContextDataProcess::get_send_body(int& result) {
    result = 1;
    if (_send_body.empty()) {
        return NULL;
    }
    
    std::string* body = new std::string(std::move(_send_body));
    _send_body.clear();
    return body;
}

void HttpContextDataProcess::msg_recv_finish() {
    if (!_handler || !_context) return;

    try {
        // ���������Ϣ��ֱ�ӷ���˽�г�Ա����Ϊ�� friend��
        auto& req_head = _base_process->get_req_head_para();
        
        auto& ctx_req = _context->mutable_request();
        ctx_req.method = req_head._method;
        ctx_req.url = req_head._url_path;
        ctx_req.version = req_head._version;
        ctx_req.headers = req_head._headers;
        ctx_req.body = _recv_body;  // ����������

        // ���ô�����
        detail::HandlerContextScope scope(this);
        _handler->on_http_request(*_context);

        // ������Ӧ���䵽 res_head
        auto& res_head = _base_process->get_res_head_para();
        
        const auto& ctx_res = _context->response();
        res_head._response_code = ctx_res.status;
        res_head._response_str = ctx_res.reason;
        res_head._headers = ctx_res.headers;

        // ������Ӧ�嵽 _send_body
        _send_body = ctx_res.body;
        
        // �Զ����� Content-Length
        if (!_send_body.empty()) {
            res_head._headers["Content-Length"] = std::to_string(_send_body.size());
        } else {
            res_head._headers["Content-Length"] = "0";
        }

        // ȷ���� Connection ͷ
        if (res_head._headers.find("Connection") == res_head._headers.end()) {
            res_head._headers["Connection"] = "close";
        }
    } catch (const std::exception& e) {
        // ������
        if (_handler) {
            _handler->on_error(std::string("HTTP handler exception: ") + e.what());
        }

        // ���� 500 ����
        auto& res_head = _base_process->get_res_head_para();
        res_head._response_code = 500;
        res_head._response_str = "Internal Server Error";
        res_head._headers.clear();
        res_head._headers["Content-Type"] = "text/plain; charset=utf-8";
        res_head._headers["Content-Length"] = "21";
        res_head._headers["Connection"] = "close";

        // ע����ʱ������ body����ܻ��Զ�����
        // std::string error_body = "Internal Server Error";
        // TODO: ��ȷ������Ӧ body
    }
}

void HttpContextDataProcess::complete_async_response() {
    // 同步异步回调中设置的响应数据到发送缓冲区
    if (_context) {
        auto& res_head = _base_process->get_res_head_para();

        // 更新响应状态和头
        const auto& ctx_res = _context->response();
        res_head._response_code = ctx_res.status;
        res_head._response_str = ctx_res.reason;
        res_head._headers = ctx_res.headers;

        // 同步响应体
        _send_body = ctx_res.body;

        // 自动设置 Content-Length
        if (!_send_body.empty()) {
            res_head._headers["Content-Length"] = std::to_string(_send_body.size());
        } else {
            res_head._headers["Content-Length"] = "0";
        }

        PDEBUG("[HttpContextDataProcess] Async response completed: status=%d, body_size=%zu",
               res_head._response_code, _send_body.size());
    }

    // 调用基类方法清除标志并通知发送
    http_base_data_process::complete_async_response();
}

void HttpContextDataProcess::handle_timeout(std::shared_ptr<::timer_msg>& t_msg) {
    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->handle_timeout(t_msg);
    }
    http_base_data_process::handle_timeout(t_msg);
}

void HttpContextDataProcess::handle_msg(std::shared_ptr<::normal_msg>& msg) {
    if (!msg) {
        return;
    }

    if (msg->_msg_op == HTTP_CONTEXT_TASK_MSG_OP) {
        auto task_msg = std::dynamic_pointer_cast<HttpContextTaskMessage>(msg);
        if (task_msg && task_msg->task && _context) {
            detail::HandlerContextScope scope(this);
            task_msg->task(*_context);
        }
        return;
    }

    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->handle_thread_msg(msg);
    }
}

void HttpContextImpl::add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) {
    if (_conn && t_msg) {
        if (timeout_ms > 0) {
            uint64_t clamped = std::min<uint64_t>(timeout_ms, std::numeric_limits<uint32_t>::max());
            t_msg->_time_length = static_cast<uint32_t>(clamped);
        }
        _conn->add_timer(t_msg);
    }
}

void HttpContextImpl::send_msg(std::shared_ptr<::normal_msg> msg) {
    if (_conn && msg) {
        ObjId target = _conn->get_id();
        base_net_thread::put_obj_msg(target, msg);
    }
}

} // namespace myframe
