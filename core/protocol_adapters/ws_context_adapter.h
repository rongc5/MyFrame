// MyFrame Unified Protocol Architecture - WebSocket Level 2 Context Adapter
// Implements WsContext for protocol-level control
#ifndef __WS_CONTEXT_ADAPTER_H__
#define __WS_CONTEXT_ADAPTER_H__

#include "../protocol_context.h"
#include "../web_socket_res_process.h"
#include "../web_socket_data_process.h"
#include <memory>
#include <map>
#include <queue>

namespace myframe {

// Forward declarations
class WsContextDataProcess;

// ============================================================================
// WebSocket Context Implementation
// �ṩ WebSocket Э��ϸ�ڷ��ʺͿ���
// ============================================================================

class WsContextImpl : public WsContext {
public:
    WsContextImpl(web_socket_process* process, std::shared_ptr<base_net_obj> conn);
    virtual ~WsContextImpl();

    // WsContext �ӿ�ʵ��
    const WsFrame& frame() const override;

    void send_text(const std::string& text) override;
    void send_binary(const void* data, size_t len) override;
    void send_ping() override;
    void send_pong() override;
    void close(uint16_t code = 1000, const std::string& reason = "") override;

    bool is_closing() const override;
    size_t pending_frames() const override;

    void broadcast(const std::string& message) override;
    void broadcast_except_self(const std::string& message) override;

    void set_user_data(const std::string& key, void* data) override;
    void* get_user_data(const std::string& key) const override;

    // ========== �ײ���� ==========
    // ��ȡԭʼ�������Ӷ���
    virtual std::shared_ptr<base_net_obj> raw_connection() override;

    // ========== �̷߳��� ==========
    // ��ȡ��ǰ�����߳�
    base_net_thread* get_thread() const override;

    // Timer and message management
    void add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) override;
    void send_msg(std::shared_ptr<::normal_msg> msg) override;

    // �ڲ��ӿ�
    void set_frame(const WsFrame& frame) { _frame = frame; }
    web_socket_process* get_process() { return _process; }

private:
    web_socket_process* _process;
    std::shared_ptr<base_net_obj> _conn;
    WsFrame _frame;
    std::map<std::string, void*> _user_data;
    bool _is_closing;
    std::queue<WsFrame> _pending_frames;
};

// ============================================================================
// WebSocket Context Adapter (Level 2)
// �� web_socket_res_process ���䵽 IProtocolHandler �ӿ�
// ============================================================================

class WsContextAdapter : public web_socket_res_process {
public:
    WsContextAdapter(
        std::shared_ptr<base_net_obj> conn,
        IProtocolHandler* handler);

    virtual ~WsContextAdapter();

    // ������������ UnifiedProtocolFactory ʹ�ã�
    static std::unique_ptr<base_data_process> create(
        std::shared_ptr<base_net_obj> conn,
        IProtocolHandler* handler);

private:
    IProtocolHandler* _handler;
};

// ============================================================================
// WebSocket Context Data Process
// WebSocket ���ݴ����㣬���� IProtocolHandler::on_ws_frame()
// ============================================================================

class WsContextDataProcess : public web_socket_data_process {
public:
    WsContextDataProcess(
        web_socket_process* process,
        IProtocolHandler* handler);

    virtual ~WsContextDataProcess();

    void msg_recv_finish() override;
    void on_connect();
    void on_close() override;

private:
    IProtocolHandler* _handler;
    std::shared_ptr<WsContextImpl> _context;
};

} // namespace myframe

#endif // __WS_CONTEXT_ADAPTER_H__
