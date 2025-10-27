// MyFrame Unified Protocol Architecture - Level 2 Context
// Protocol-level context for advanced scenarios
#ifndef __PROTOCOL_CONTEXT_H__
#define __PROTOCOL_CONTEXT_H__

#include "app_handler_v2.h"
#include "base_net_obj.h"
#include "base_data_process.h"
#include "base_net_thread.h"
#include "common_def.h"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace myframe {

// ============================================================================
// Level 2: Protocol Context Handler (Э���)
// �ṩЭ��ϸ�ڷ��ʺͿ��ƣ�֧���첽����ʽ��״̬��
// ============================================================================

// ============================================================================
// HTTP Э��������
// ============================================================================

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
    virtual void async_response(std::function<void()> fn) = 0;

    // ����첽��Ӧ��ɲ�֪ͨ���ͣ����� set_async_response_pending(false) + notify_send_ready()��
    virtual void complete_async_response() = 0;

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
    virtual void set_user_data(const std::string& key, void* data) = 0;

    // ��ȡ�û��Զ�������
    virtual void* get_user_data(const std::string& key) const = 0;

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

    const ObjId& id = net->get_id();
    return base_net_thread::get_base_net_thread_obj(id._thread_index);
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
