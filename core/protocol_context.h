// MyFrame Unified Protocol Architecture - Level 2 Context
// Protocol-level context for advanced scenarios
#ifndef __PROTOCOL_CONTEXT_H__
#define __PROTOCOL_CONTEXT_H__

#include "app_handler_v2.h"
#include "base_net_obj.h"
#include "base_data_process.h"
#include "base_net_thread.h"
#include "common_obj_container.h"
#include "common_def.h"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <typeindex>
#include <utility>
#include <type_traits>
#include <exception>

// C++11 兼容的弃用标注（[[deprecated]] 是 C++14）
#ifndef MYFRAME_DEPRECATED
#  if __cplusplus >= 201402L
#    define MYFRAME_DEPRECATED(msg) [[deprecated(msg)]]
#  elif defined(__GNUC__) || defined(__clang__)
#    define MYFRAME_DEPRECATED(msg) __attribute__((deprecated(msg)))
#  else
#    define MYFRAME_DEPRECATED(msg)
#  endif
#endif

namespace myframe {

// ============================================================================
// Level 2: Protocol Context Handler (Э���)
// �ṩЭ��ϸ�ڷ��ʺͿ��ƣ�֧���첽����ʽ��״̬��
// ============================================================================

// ============================================================================
// HTTP Э��������
// ============================================================================

// 两阶段异步内部载体：承载 work() 的成功值或异常。
// 用 shared_ptr<R> 存放结果，不要求 R 可默认构造（成功前为空指针）。
template <class R>
struct MfAsyncHolder {
    std::exception_ptr error;       // 非空表示 work 抛异常
    std::shared_ptr<R> value;       // 非空表示成功
    void run(std::function<R()>& work) {
        try { value = std::make_shared<R>(work()); }
        catch (...) { error = std::current_exception(); }
    }
};
template <>
struct MfAsyncHolder<void> {
    std::exception_ptr error;
    void run(std::function<void()>& work) {
        try { work(); } catch (...) { error = std::current_exception(); }
    }
};

class HttpContext;   // 供中间件 typedef 前置引用

// 请求完成原因（after 钩子 = 请求结束钩子；契约见 docs/http-router-rfc.md §4）。
// v1 只产生 Success/Error/ShortCircuited；Disconnected 保留、暂不触发（断线不跑 after）。
enum class CompletionReason { Success, Error, ShortCircuited, Disconnected };
// before：返回 false = 短路（须已设响应，否则框架兜底 500）。after：请求结束钩子，逆序、最多一次。
typedef std::function<bool(HttpContext&)> BeforeMiddleware;
typedef std::function<void(HttpContext&, CompletionReason)> AfterMiddleware;

class HttpContext {
public:
    virtual ~HttpContext() = default;

    // ========== ����/��Ӧ���� ==========

    // ��ȡ�������ֻ����
    virtual const HttpRequest& request() const = 0;

    // ��ȡ��Ӧ���󣨶�д��
    virtual HttpResponse& response() = 0;

    // ========== �߼����� ==========

    // �첽��Ӧ - ����һ���߳���ִ�� fn��Ȼ������Ӧ
    // [已弃用] 单阶段异步：回调在工作线程执行、易捕获裸 ctx 造成悬空/跨线程改 response。
    MYFRAME_DEPRECATED("use two-phase async_response(work, done) instead")
    virtual void async_response(std::function<void()> fn) = 0;

    // 两阶段异步（安全，默认安全语义）：
    //   work:       工作线程执行，只算结果，绝不接触 Context；
    //   on_success: work 成功后回投所属网络线程执行，收到结果值 R。
    // 框架自动兜底（不需要业务处理错误路径）：
    //   - work 抛异常  → 500，不调用 on_success；
    //   - 队列拒绝     → 503，不调用 on_success；
    //   - on_success 自身抛异常 → 500。
    // 响应恰好 finalize 一次（含 Keep-Alive 换请求/连接断开的丢弃）。
    // 注意：R != void 时需显式传 std::function<R()>（lambda 不会自动推导为 std::function）。
    template <class R>
    void async_response(std::function<R()> work,
                        std::function<void(HttpContext&, R)> on_success) {
        std::shared_ptr<MfAsyncHolder<R> > holder = std::make_shared<MfAsyncHolder<R> >();
        std::function<void()> work_thunk =
            [holder, work]() mutable { holder->run(work); };
        // apply 在网络线程执行：work 有异常则重新抛出（由 finalize_once 兜底成 500），
        // 否则调用业务 on_success。
        std::function<void(HttpContext&)> apply_thunk =
            [holder, on_success](HttpContext& ctx) {
                if (holder->error) std::rethrow_exception(holder->error);
                on_success(ctx, *holder->value);
            };
        async_response_erased(std::move(work_thunk), std::move(apply_thunk));
    }

    // void 结果重载（无返回值）。
    void async_response(std::function<void()> work,
                        std::function<void(HttpContext&)> on_success) {
        std::shared_ptr<MfAsyncHolder<void> > holder = std::make_shared<MfAsyncHolder<void> >();
        std::function<void()> work_thunk =
            [holder, work]() mutable { holder->run(work); };
        std::function<void(HttpContext&)> apply_thunk =
            [holder, on_success](HttpContext& ctx) {
                if (holder->error) std::rethrow_exception(holder->error);
                on_success(ctx);
            };
        async_response_erased(std::move(work_thunk), std::move(apply_thunk));
    }

    // 类型擦除落点（模板不能是虚函数）。实现见 HttpContextImpl。
    //   work_thunk : 工作线程执行，绝不抛异常（异常已被 holder 吞入）；
    //   apply_thunk: 网络线程执行，成功则应用结果、失败则抛异常（框架兜底 500）。
    virtual void async_response_erased(std::function<void()> work_thunk,
                                       std::function<void(HttpContext&)> apply_thunk) = 0;

    // ����첽��Ӧ��ɲ�֪ͨ���ͣ����� set_async_response_pending(false) + notify_send_ready()��
    virtual void complete_async_response() = 0;

    // ===== 中间件 after 生命周期（请求结束钩子；由 MiddlewareChain 驱动）=====
    // 进入某 before 时登记其 after（含短路的那个）；request-scoped，每请求 reset 清空。
    virtual void push_after(AfterMiddleware after) = 0;
    // 逆序执行已登记的 after，最多一次；单个 after 抛异常记录并继续、不改写响应、不阻断。
    // reason==Disconnected 时 v1 不执行（断线不跑 after）。
    virtual void run_after_chain(CompletionReason reason) = 0;
    // handler 返回后是否处于异步挂起：是则 after 交由 finalize_once 在真正完成时执行。
    virtual bool async_pending() const = 0;

    // ������ʽ��Ӧģʽ
    virtual void enable_streaming() = 0;

    // ��ʽд������
    virtual size_t stream_write(const void* data, size_t len) = 0;

    // Э���������� HTTP ������ WebSocket��
    virtual void upgrade_to_websocket() = 0;

    // ========== ���ӿ��� ==========

    // �ر�����
    virtual void close() = 0;

    // �������ӳ�ʱ�����룩
    virtual void set_timeout(uint64_t ms) = 0;

    // ����/���� Keep-Alive
    virtual void keep_alive(bool enable) = 0;

    // ========== �û����ݴ洢 ==========

    // �洢�û��Զ�������
    MYFRAME_DEPRECATED("use set_ext<T>() instead")
    virtual void set_user_data(const std::string& key, void* data) = 0;

    MYFRAME_DEPRECATED("use get_ext<T>() instead")
    virtual void* get_user_data(const std::string& key) const = 0;

    // ===== 类型安全请求扩展（拥有对象；每请求 reset_for_request 清空）=====
    template <class T, class... A>
    T& set_ext(const std::string& key, A&&... args) {
        typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type S;
        std::shared_ptr<S> p = std::make_shared<S>(std::forward<A>(args)...);
        set_ext_erased(key, std::type_index(typeid(S)), std::static_pointer_cast<void>(p));
        return *p;
    }
    template <class T>
    T* get_ext(const std::string& key) const {
        typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type S;
        std::shared_ptr<void> v = get_ext_erased(key, std::type_index(typeid(S)));
        return v ? static_cast<S*>(v.get()) : nullptr;
    }
    virtual void set_ext_erased(const std::string& key, std::type_index ti, std::shared_ptr<void> val) = 0;
    virtual std::shared_ptr<void> get_ext_erased(const std::string& key, std::type_index ti) const = 0;
    // 当前请求代号（Keep-Alive 每请求 ++；异步回投据此判断是否已换请求）
    virtual uint64_t request_generation() const = 0;

    // ===== 路由参数 / HEAD body 抑制（由 RoutingHandler 分发层写入；request-scoped）=====
    // 捕获的 {param}（Router 匹配后由分发层注入）；不存在返回空串。
    virtual const std::string& route_param(const std::string& key) const = 0;
    virtual void set_route_params(const std::map<std::string, std::string>& params) = 0;
    // HEAD：复用 GET handler 但发送阶段抑制 body（保留状态/headers/Content-Length）。
    virtual void set_suppress_body(bool on) = 0;
    virtual bool suppress_body() const = 0;

    // ========== �ײ���� ==========

    // ��ȡ������Ϣ
    virtual ConnectionInfo& connection_info() = 0;

    // ��ȡԭʼ�������Ӷ���
    virtual std::shared_ptr<base_net_obj> raw_connection() = 0;

    // ========== �̷߳��� ==========
    // ��ȡ��ǰҵ�����߳�
    virtual ::base_net_thread* get_thread() const = 0;

    // ========== 定时器和消息管理 ==========
    // 添加定时器（milliseconds 为单位）
    virtual void add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) = 0;

    // 发送自定义消息（用于线程之间通信）
    virtual void send_msg(std::shared_ptr<::normal_msg> msg) = 0;
};

// 统一的 HTTP 异步任务消息编号（'H''C''T''1'）
constexpr int HTTP_CONTEXT_TASK_MSG_OP = 0x48435431;

// 用于在网络线程执行回调的通用消息封装
struct HttpContextTaskMessage : public ::normal_msg {
    HttpContextTaskMessage()
        : ::normal_msg(HTTP_CONTEXT_TASK_MSG_OP) {}

    explicit HttpContextTaskMessage(std::function<void(HttpContext&)> fn)
        : ::normal_msg(HTTP_CONTEXT_TASK_MSG_OP), task(std::move(fn)) {}

    std::function<void(HttpContext&)> task;
};

// ============================================================================
// WebSocket Э��������
// ============================================================================

class WsContext {
public:
    virtual ~WsContext() = default;

    // ========== ֡���� ==========

    // ��ȡ��ǰ�յ���֡
    virtual const WsFrame& frame() const = 0;

    // ========== ���Ϳ��� ==========

    // �����ı�֡
    virtual void send_text(const std::string& text) = 0;

    // ���Ͷ�����֡
    virtual void send_binary(const void* data, size_t len) = 0;

    // ���� PING ֡
    virtual void send_ping() = 0;

    // ���� PONG ֡
    virtual void send_pong() = 0;

    // ���� CLOSE ֡���ر�����
    // code: WebSocket �ر��루�� 1000 = Normal Closure��
    // reason: �ر�ԭ��
    virtual void close(uint16_t code = 1000, const std::string& reason = "") = 0;

    // ========== ״̬��ѯ ==========

    // ����Ƿ����ڹر�
    virtual bool is_closing() const = 0;

    // ��ȡ������֡������
    virtual size_t pending_frames() const = 0;

    // ========== �㲥֧�֣���Ҫ��� Hub�� ==========

    // �㲥��Ϣ����������
    virtual void broadcast(const std::string& message) = 0;

    // �㲥��Ϣ�����Լ�֮�����������
    virtual void broadcast_except_self(const std::string& message) = 0;

    // ========== �û����ݴ洢 ==========

    // �洢�û��Զ�������
    virtual void set_user_data(const std::string& key, void* data) = 0;

    // ��ȡ�û��Զ�������
    virtual void* get_user_data(const std::string& key) const = 0;

    // ========== �ײ���� ==========

    // ��ȡԭʼ�������Ӷ���
    virtual std::shared_ptr<base_net_obj> raw_connection() = 0;

    // ========== �̷߳��� ==========
    // ��ȡ��ǰ WebSocket �����߳�
    virtual ::base_net_thread* get_thread() const = 0;

    // ========== 定时器和消息管理 ==========
    // 添加定时器（milliseconds 为单位）
    virtual void add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) = 0;

    // 发送自定义消息（用于线程之间通信）
    virtual void send_msg(std::shared_ptr<::normal_msg> msg) = 0;
};

// ============================================================================
// ������Э��������
// ============================================================================

class BinaryContext {
public:
    // Э��״̬��
    enum State {
        HANDSHAKE = 0,   // ���ֽ׶�
        READY = 1,       // �������ɴ���ҵ��
        STREAMING = 2,   // ��ʽ����ģʽ
        CLOSING = 3      // ���ڹر�
    };

    virtual ~BinaryContext() = default;

    // ========== ״̬���� ==========

    // ��ȡ��ǰ״̬
    virtual State state() const = 0;

    // ����״̬
    virtual void set_state(State s) = 0;

    // ========== ��Ϣ���� ==========

    // ��ȡЭ�� ID
    virtual uint32_t protocol_id() const = 0;

    // ��ȡ����ָ��
    virtual const uint8_t* payload() const = 0;

    // ��ȡ���ش�С
    virtual size_t payload_size() const = 0;

    // ========== ��Ӧ���� ==========

    // ������Ӧ��Ϣ
    virtual void send(uint32_t proto_id, const void* data, size_t len) = 0;

    // �첽���ͣ�ִ�� fn ���ͣ�
    virtual void send_async(std::function<void()> fn) = 0;

    // ========== ��ʽ���� ==========

    // ������ʽģʽ�����ڴ����ݴ��䣩
    virtual void enable_streaming() = 0;

    // ��ʽд������
    virtual size_t stream_write(const void* data, size_t len) = 0;

    // ========== �û����ݴ洢 ==========

    // �洢�û��Զ�������
    virtual void set_user_data(const std::string& key, void* data) = 0;

    // ��ȡ�û��Զ�������
    virtual void* get_user_data(const std::string& key) const = 0;

    // ========== �ײ���� ==========

    // ��ȡԭʼ�������Ӷ���
    virtual std::shared_ptr<base_net_obj> raw_connection() = 0;

    // ========== �̷߳��� ==========
    // ��ȡ��ǰ������Э�鴦���߳�
    virtual ::base_net_thread* get_thread() const = 0;

    // ========== 定时器和消息管理 ==========
    // 添加定时器（milliseconds 为单位）
    virtual void add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) = 0;

    // 发送自定义消息（用于线程之间通信）
    virtual void send_msg(std::shared_ptr<::normal_msg> msg) = 0;
};

// ============================================================================
// Level 2 Э�鴦�����ӿ�
// ============================================================================

class IProtocolHandler {
public:
    virtual ~IProtocolHandler() {}

    // HTTP ������
    virtual void on_http_request(HttpContext& ctx) {
        ctx.response().status = 501;
        ctx.response().reason = "Not Implemented";
        ctx.response().set_text("HTTP handler not implemented");
    }

    // WebSocket ֡����
    virtual void on_ws_frame(WsContext& ctx) {
        // Ĭ�ϻ���
        auto& frame = ctx.frame();
        if (frame.opcode == WsFrame::TEXT) {
            ctx.send_text(frame.payload);
        }
    }

    // ��������Ϣ����
    virtual void on_binary_message(BinaryContext& ctx) {
        (void)ctx;
    }

    // �������ڹ���

    // ���ӽ���
    virtual void on_connect(const ConnectionInfo& info) {
        (void)info;
    }

    // ���ӶϿ�
    virtual void on_disconnect() {
        // Ĭ�ϲ�ʵ��
    }

    // ������
    virtual void on_error(const std::string& error) {
        (void)error;
    }

    // ========== ��Ϣ�ͳ�ʱ���� ==========

    // ������Ϣ�������첽���������߳�ͨ�ŵȣ�
    virtual void handle_msg(std::shared_ptr<::normal_msg>& msg) {
        (void)msg;
    }

    virtual bool handle_thread_msg(std::shared_ptr<::normal_msg>& msg) {
        handle_msg(msg);
        return true;
    }

    // ������ʱ�����ڶ�ʱ������ʱ���Ƶȣ�
    virtual void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) {
        (void)t_msg;
    }

    // ========== �̷߳��� ==========
    // ��ȡ��ǰҵ�����߳�
    // ���� nullptr ��ʾ�߳���Ϣ������
    virtual ::base_net_thread* get_current_thread() const;

    // ========== ����ʱ������ ==========
    uint32_t schedule_timeout(uint32_t delay_ms, uint32_t timer_type = APPLICATION_TIMER_TYPE) const;

    // ========== ��������ʶ ==========
    ObjId current_connection_id() const;
};

inline uint32_t IProtocolHandler::schedule_timeout(uint32_t delay_ms, uint32_t timer_type) const {
    if (delay_ms == 0) {
        delay_ms = 1;
    }

    ::base_data_process* process = detail::current_process();
    if (!process) {
        return 0;
    }

    std::shared_ptr<base_net_obj> net = process->get_base_net();
    if (!net) {
        return 0;
    }

    std::shared_ptr<timer_msg> timer(new timer_msg);
    timer->_obj_id = net->get_id()._id;
    timer->_timer_type = timer_type ? timer_type : APPLICATION_TIMER_TYPE;
    timer->_time_length = delay_ms;

    process->add_timer(timer);
    return timer->_timer_id;
}

inline ::base_net_thread* IProtocolHandler::get_current_thread() const {
    ::base_data_process* process = detail::current_process();
    if (!process) {
        return nullptr;
    }

    std::shared_ptr<base_net_obj> net = process->get_base_net();
    if (!net) {
        return nullptr;
    }

    auto* container = net->get_net_container();
    return container ? container->get_owner_thread() : nullptr;
}

inline ObjId IProtocolHandler::current_connection_id() const {
    ::base_data_process* process = detail::current_process();
    if (!process) {
        return ObjId{};
    }

    std::shared_ptr<base_net_obj> net = process->get_base_net();
    if (!net) {
        return ObjId{};
    }
    return net->get_id();
}

} // namespace myframe

#endif // __PROTOCOL_CONTEXT_H__
