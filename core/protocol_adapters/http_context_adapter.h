// MyFrame Unified Protocol Architecture - HTTP Level 2 Context Adapter
// Implements HttpContext for protocol-level control
#ifndef __HTTP_CONTEXT_ADAPTER_H__
#define __HTTP_CONTEXT_ADAPTER_H__

#include "../protocol_context.h"
#include "../http_res_process.h"
#include "../http_base_data_process.h"
#include <memory>
#include <map>
#include <unordered_map>
#include <typeindex>
#include <utility>
#include <thread>
#include <queue>

namespace myframe {

// Forward declarations
class HttpContextDataProcess;

// ============================================================================
// HTTP Context Implementation
// �ṩЭ��ϸ�ڷ��ʺͿ���
// ============================================================================

class HttpContextImpl : public HttpContext {
public:
    HttpContextImpl(http_base_process* process, std::shared_ptr<base_net_obj> conn);
    virtual ~HttpContextImpl();

    // HttpContext �ӿ�ʵ��
    const HttpRequest& request() const override;
    HttpResponse& response() override;

    void async_response(std::function<void()> fn) override;
    void async_response_erased(std::function<void()> work_thunk,
                               std::function<void(HttpContext&)> apply_thunk) override;
    void complete_async_response() override;

    // 中间件 after 生命周期
    void push_after(AfterMiddleware after) override;
    void run_after_chain(CompletionReason reason) override;
    bool async_pending() const override;

    // 网络线程统一完成出口（finalize-once）：
    //   - gen != 当前代号 → 已换请求（Keep-Alive 下一请求），丢弃；
    //   - _state 已 Finalized → 幂等，直接返回；
    //   - 否则置 Finalized，执行 apply（apply 抛异常则回落 500 且 reason=Error），
    //     再逆序跑 after（发送前），最后触发发送。
    // 必须在所属网络线程调用。
    void finalize_once(uint64_t gen, const std::function<void(HttpContext&)>& apply,
                       CompletionReason reason = CompletionReason::Success);
    void enable_streaming() override;
    size_t stream_write(const void* data, size_t len) override;
    void upgrade_to_websocket() override;

    void close() override;
    void set_timeout(uint64_t ms) override;
    void keep_alive(bool enable) override;

    void set_user_data(const std::string& key, void* data) override;
    void* get_user_data(const std::string& key) const override;
    void set_ext_erased(const std::string& key, std::type_index ti, std::shared_ptr<void> val) override;
    std::shared_ptr<void> get_ext_erased(const std::string& key, std::type_index ti) const override;
    uint64_t request_generation() const override;
    // 内部：新请求开始时重置（清 response/extensions/user_data/streaming，++generation）
    void reset_for_request();

    ConnectionInfo& connection_info() override;
    std::shared_ptr<base_net_obj> raw_connection() override;

    base_net_thread* get_thread() const override;

    // Timer and message management
    void add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) override;
    void send_msg(std::shared_ptr<::normal_msg> msg) override;

    // Internal helpers for adapters
    HttpRequest& mutable_request() { return _request; }
    HttpResponse& mutable_response() { return _response; }
    http_base_process* get_process() { return _process; }

private:
    // 请求生命周期状态（finalize-once 依据；仅在网络线程读写）
    enum class RequestState { Handling, AsyncPending, Finalized };
    // 实际触发发送（notify）；由 finalize_once/complete_async_response 幂等地各调一次
    void do_notify_complete();

    http_base_process* _process;
    // FIX(mem-leak): weak to break ref-cycle conn->unique_ptr<HttpContextAdapter>->_context->_conn->conn
    std::weak_ptr<base_net_obj> _conn;
    HttpRequest _request;
    HttpResponse _response;
    std::map<std::string, void*> _user_data;
    std::unordered_map<std::string, std::pair<std::type_index, std::shared_ptr<void> > > _ext;
    uint64_t _request_generation;
    RequestState _state;
    std::vector<AfterMiddleware> _after_stack;   // 已进入的 after（request-scoped）
    bool _after_ran;                             // after 链是否已执行（最多一次）
    ConnectionInfo _conn_info;
    bool _streaming_enabled;
};

// ============================================================================
// HTTP Context Adapter (Level 2)
// �� http_res_process ���䵽 IProtocolHandler �ӿ�
// ============================================================================

class HttpContextAdapter : public http_res_process {
public:
    HttpContextAdapter(
        std::shared_ptr<base_net_obj> conn,
        IProtocolHandler* handler);

    virtual ~HttpContextAdapter();

    // ������������ UnifiedProtocolFactory ʹ�ã�
    static std::unique_ptr<base_data_process> create(
        std::shared_ptr<base_net_obj> conn,
        IProtocolHandler* handler);

private:
    IProtocolHandler* _handler;
};

// ============================================================================
// HTTP Context Data Process
// HTTP ���ݴ����㣬���� IProtocolHandler::on_http_request()
// ============================================================================

class HttpContextDataProcess : public http_base_data_process {
public:
    HttpContextDataProcess(
        http_base_process* process,
        IProtocolHandler* handler);

    virtual ~HttpContextDataProcess();

    // ��д�ؼ��麯��
    size_t process_recv_body(const char* buf, size_t len, int& result) override;
    std::string* get_send_head() override;
    std::string* get_send_body(int& result) override;
    void msg_recv_finish() override;
    void complete_async_response() override;
    void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) override;
    void handle_msg(std::shared_ptr<::normal_msg>& msg) override;

private:
    IProtocolHandler* _handler;
    std::shared_ptr<HttpContextImpl> _context;
    std::string _recv_body;   // �洢������
    std::string _send_body;   // �洢��Ӧ��
};

} // namespace myframe

#endif // __HTTP_CONTEXT_ADAPTER_H__
