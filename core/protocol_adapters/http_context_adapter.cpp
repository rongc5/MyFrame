// MyFrame Unified Protocol Architecture - HTTP Context Adapter Implementation
#include "http_context_adapter.h"
#include "../task_executor.h"
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
    : _process(process), _conn(conn), _request_generation(0),
      _state(RequestState::Handling), _after_ran(false), _suppress_body(false),
      _streaming_enabled(false) {
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

void HttpContextImpl::set_ext_erased(const std::string& key, std::type_index ti, std::shared_ptr<void> val) {
    std::unordered_map<std::string, std::pair<std::type_index, std::shared_ptr<void> > >::iterator it = _ext.find(key);
    if (it != _ext.end()) { it->second.first = ti; it->second.second = std::move(val); }
    else { _ext.emplace(key, std::make_pair(ti, std::move(val))); }
}

std::shared_ptr<void> HttpContextImpl::get_ext_erased(const std::string& key, std::type_index ti) const {
    std::unordered_map<std::string, std::pair<std::type_index, std::shared_ptr<void> > >::const_iterator it = _ext.find(key);
    if (it == _ext.end() || it->second.first != ti) return std::shared_ptr<void>();
    return it->second.second;
}

uint64_t HttpContextImpl::request_generation() const { return _request_generation; }

// 新请求开始时重置：清 response/extensions/user_data/streaming，++generation。
// 杜绝 Keep-Alive 下上一请求的租户身份/响应残留泄漏进下一请求。
void HttpContextImpl::reset_for_request() {
    _request = HttpRequest();     // 清请求（含 body/headers），防 Keep-Alive 跨请求泄漏
    _response = HttpResponse();
    _ext.clear();
    _user_data.clear();
    _streaming_enabled = false;
    _state = RequestState::Handling;
    _after_stack.clear();       // 清中间件 after 栈（request-scoped，防跨请求）
    _after_ran = false;
    _route_params.clear();      // 清路由参数（request-scoped）
    _suppress_body = false;
    ++_request_generation;
}

const std::string& HttpContextImpl::route_param(const std::string& key) const {
    static const std::string empty;
    std::map<std::string, std::string>::const_iterator it = _route_params.find(key);
    return it == _route_params.end() ? empty : it->second;
}
void HttpContextImpl::set_route_params(const std::map<std::string, std::string>& params) {
    _route_params = params;
}
void HttpContextImpl::set_suppress_body(bool on) { _suppress_body = on; }
bool HttpContextImpl::suppress_body() const { return _suppress_body; }

const HttpRequest& HttpContextImpl::request() const {
    return _request;
}

HttpResponse& HttpContextImpl::response() {
    return _response;
}

void HttpContextImpl::async_response(std::function<void()> fn) {
    // [已弃用] 单阶段异步：fn 在工作线程执行且可能接触裸 ctx（不安全）。
    // 兼容期改用有界执行器（不再每请求 detach 新线程）；请迁移到两阶段 async_response(work, on_success)。
    if (_process) {
        auto* data_process = _process->get_process();
        if (data_process) {
            auto* http_data_process = dynamic_cast<http_base_data_process*>(data_process);
            if (http_data_process) http_data_process->set_async_response_pending(true);
        }
    }
    if (!fn) return;
    bool submitted = default_task_executor().submit([fn]() { fn(); });
    if (!submitted) {
        // 执行器拒绝：直接完成（幂等），避免永远 pending
        _response = HttpResponse();
        _response.status = 503;
        _response.reason = "Service Unavailable";
        complete_async_response();
    }
}

void HttpContextImpl::async_response_erased(std::function<void()> work_thunk,
                                            std::function<void(HttpContext&)> apply_thunk) {
    // 标记异步挂起，阻止立即发送
    if (_process) {
        auto* dp = _process->get_process();
        auto* hdp = dp ? dynamic_cast<http_base_data_process*>(dp) : nullptr;
        if (hdp) hdp->set_async_response_pending(true);
    }
    _state = RequestState::AsyncPending;
    uint64_t gen = _request_generation;

    auto c = _conn.lock();
    if (!c) { finalize_once(gen, nullptr, CompletionReason::Disconnected); return; }   // 连接已不在：finalize 且 v1 不跑 after

    ObjId objid = c->get_id();
    bool submitted = default_task_executor().submit(
        [work_thunk, apply_thunk, objid, gen]() {
            work_thunk();   // 工作线程：只算结果、绝不接触 Context、绝不抛异常
            std::shared_ptr<::normal_msg> tmsg =
                std::make_shared<HttpContextTaskMessage>(
                    [apply_thunk, gen](HttpContext& ctx) {
                        // 网络线程统一出口：代号校验 + finalize-once + 异常兜底
                        static_cast<HttpContextImpl&>(ctx).finalize_once(gen, apply_thunk);
                    });
            ObjId t = objid;   // put_obj_msg 取 ObjId&，需非 const 左值
            base_net_thread::put_obj_msg(t, tmsg);   // 按连接 ID 投回网络线程；连接没了自动丢弃
        });

    if (!submitted) {
        // 队列拒绝 → 503（此刻仍在网络线程，经统一出口完成；reason=Error 让 after 跑）
        finalize_once(gen, [](HttpContext& ctx) {
            ctx.response() = HttpResponse();
            ctx.response().status = 503;
            ctx.response().reason = "Service Unavailable";
        }, CompletionReason::Error);
    }
}


// 实际触发发送：set_async_response_pending(false) + notify_send_ready()。
// 幂等由调用方（_state）保证，这里不再判重。
void HttpContextImpl::do_notify_complete() {
    if (_process) {
        auto* data_process = _process->get_process();
        if (data_process) {
            auto* http_data_process = dynamic_cast<http_base_data_process*>(data_process);
            if (http_data_process) {
                http_data_process->complete_async_response();
            }
        }
    }
}

// 完成异步响应并通知发送。幂等：重复调用只发送一次。
// （旧单阶段 API 与流式仍会直接调用它；两阶段路径走 finalize_once。）
void HttpContextImpl::complete_async_response() {
    if (_state == RequestState::Finalized) return;   // 幂等：已完成
    _state = RequestState::Finalized;
    do_notify_complete();
}

// 网络线程统一完成出口：代号校验 + finalize-once + apply 异常兼底 500。
void HttpContextImpl::finalize_once(uint64_t gen, const std::function<void(HttpContext&)>& apply,
                                    CompletionReason reason) {
    if (gen != _request_generation) return;          // 已换请求（Keep-Alive 下一请求）→ 丢弃陈旧结果
    if (_state == RequestState::Finalized) return;   // 已完成 → 幂等
    _state = RequestState::Finalized;
    CompletionReason final_reason = reason;
    try {
        if (apply) apply(*this);                     // 成功回调 / rethrow work 异常
    } catch (...) {
        // work 异常或 on_success 自身抛异常 → 默认安全：500
        _response = HttpResponse();
        _response.status = 500;
        _response.reason = "Internal Server Error";
        final_reason = CompletionReason::Error;
    }
    run_after_chain(final_reason);                   // after 在“响应已定、字节未发”时逆序执行
    do_notify_complete();                            // 恰好触发一次发送
}

// 登记进入的 after（含短路那个）；request-scoped，reset 清空。
void HttpContextImpl::push_after(AfterMiddleware after) {
    if (after) _after_stack.push_back(std::move(after));
}

// 逆序执行 after，最多一次；单个 after 抛异常记录并继续、不改写响应、不阻断 finalize。
// v1：reason==Disconnected 不执行（断线不跑 after）。
void HttpContextImpl::run_after_chain(CompletionReason reason) {
    if (_after_ran) return;
    _after_ran = true;
    if (reason == CompletionReason::Disconnected) return;   // v1：断线/取消不跑 after
    for (std::vector<AfterMiddleware>::reverse_iterator it = _after_stack.rbegin();
         it != _after_stack.rend(); ++it) {
        try { if (*it) (*it)(*this, reason); }
        catch (...) {
            PDEBUG("[HttpContext] after middleware threw; continuing");   // 记录并继续
        }
    }
}

bool HttpContextImpl::async_pending() const {
    return _state == RequestState::AsyncPending;
}

void HttpContextImpl::enable_streaming() {
    _streaming_enabled = true;
}

size_t HttpContextImpl::stream_write(const void* data, size_t len) {
    if (!_streaming_enabled || _conn.expired()) return 0;

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
    if (!_conn.expired()) {
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
    return _conn.lock();
}

base_net_thread* HttpContextImpl::get_thread() const {
    if (auto c = _conn.lock()) {
        auto* container = c->get_net_container();
        if (container) {
            return container->get_owner_thread();
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

    // 每个请求开始先重置上下文（Keep-Alive 复用连接时防跨请求泄漏）
    _context->reset_for_request();

    try {
        // ���������Ϣ��ֱ�ӷ���˽�г�Ա����Ϊ�� friend��
        auto& req_head = _base_process->get_req_head_para();
        
        auto& ctx_req = _context->mutable_request();
        ctx_req.method = req_head._method;
        ctx_req.url = req_head._url_path;
        ctx_req.version = req_head._version;
        ctx_req.headers = req_head._headers;
        ctx_req.body = std::move(_recv_body);  // move：转交本请求体并清空缓冲，防跨请求泄漏
        _recv_body.clear();                    // move 后置空（保险，明确语义）
        // Body suppression is a transport-level HEAD invariant. Apply it to
        // explicit HEAD handlers and non-router handlers as well as auto-HEAD.
        if (strcasecmp(ctx_req.method.c_str(), "HEAD") == 0) {
            _context->set_suppress_body(true);
        }

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
        
        bool has_content_length = false;
        for (const auto& kv : res_head._headers) {
            if (strcasecmp(kv.first.c_str(), "Content-Length") == 0) {
                has_content_length = true;
                break;
            }
        }
        if (!has_content_length) {
            if (!_send_body.empty()) {
                res_head._headers["Content-Length"] = std::to_string(_send_body.size());
            } else if (!_context->suppress_body()) {
                res_head._headers["Content-Length"] = "0";
            }
        }
        // HEAD：抑制 body（保留上面已算的 Content-Length，不发送实体）
        if (_context->suppress_body()) _send_body.clear();

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

        bool has_content_length = false;
        for (const auto& kv : res_head._headers) {
            if (strcasecmp(kv.first.c_str(), "Content-Length") == 0) {
                has_content_length = true;
                break;
            }
        }
        if (!has_content_length) {
            if (!_send_body.empty()) {
                res_head._headers["Content-Length"] = std::to_string(_send_body.size());
            } else if (!_context->suppress_body()) {
                res_head._headers["Content-Length"] = "0";
            }
        }
        // HEAD：抑制 body（保留上面已算的 Content-Length，不发送实体）
        if (_context->suppress_body()) _send_body.clear();

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
    auto c = _conn.lock();
    if (c && t_msg) {
        if (timeout_ms > 0) {
            uint64_t clamped = std::min<uint64_t>(timeout_ms, std::numeric_limits<uint32_t>::max());
            t_msg->_time_length = static_cast<uint32_t>(clamped);
        }
        c->add_timer(t_msg);
    }
}

void HttpContextImpl::send_msg(std::shared_ptr<::normal_msg> msg) {
    auto c = _conn.lock();
    if (c && msg) {
        ObjId target = c->get_id();
        base_net_thread::put_obj_msg(target, msg);
    }
}

} // namespace myframe
