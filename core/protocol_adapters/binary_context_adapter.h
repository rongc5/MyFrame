// MyFrame Unified Protocol Architecture - Binary Level 2 Context Adapter
// Implements BinaryContext for protocol-level control
#ifndef __BINARY_CONTEXT_ADAPTER_H__
#define __BINARY_CONTEXT_ADAPTER_H__

#include "../protocol_context.h"
#include "../base_data_process.h"
#include <memory>
#include <map>
#include <queue>
#include <cstring>
#include <functional>

namespace myframe {

// ============================================================================
// Binary Context Implementation
// 提供二进制协议细节访问和控制
// ============================================================================

class BinaryContextImpl : public BinaryContext {
public:
    BinaryContextImpl(std::shared_ptr<base_net_obj> conn);
    virtual ~BinaryContextImpl();

    // ========== 状态管理 ==========
    State state() const override;
    void set_state(State s) override;

    // ========== 消息访问 ==========
    uint32_t protocol_id() const override;
    const uint8_t* payload() const override;
    size_t payload_size() const override;

    // ========== 响应发送 ==========
    void send(uint32_t proto_id, const void* data, size_t len) override;
    void send_async(std::function<void()> fn) override;

    // ========== 流式传输 ==========
    void enable_streaming() override;
    size_t stream_write(const void* data, size_t len) override;

    // ========== 用户数据存储 ==========
    void set_user_data(const std::string& key, void* data) override;
    void* get_user_data(const std::string& key) const override;

    // ========== 底层访问 ==========
    std::shared_ptr<base_net_obj> raw_connection() override;
    ::base_net_thread* get_thread() const override;

    // Timer and message management
    void add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) override;
    void send_msg(std::shared_ptr<::normal_msg> msg) override;

    // 内部接口
    void set_message(uint32_t proto_id, const std::string& payload);
    bool has_pending_send() const { return !_pending_queue.empty(); }
    bool get_pending_message(uint32_t& proto_id, std::string& data);

private:
    std::shared_ptr<base_net_obj> _conn;
    State _state;
    uint32_t _proto_id;
    std::string _payload;
    std::map<std::string, void*> _user_data;
    bool _streaming_enabled;

    struct PendingMessage {
        uint32_t proto_id;
        std::string data;
    };
    std::queue<PendingMessage> _pending_queue;
};

// ============================================================================
// Binary Context Adapter (Level 2)
// 将 base_data_process 适配到 IProtocolHandler 接口
// ============================================================================

class BinaryContextAdapter : public ::base_data_process {
public:
    BinaryContextAdapter(
        std::shared_ptr<base_net_obj> conn,
        IProtocolHandler* handler);

    virtual ~BinaryContextAdapter();

    // base_data_process 接口实现
    size_t process_recv_buf(const char* buf, size_t len) override;
    std::string* get_send_buf() override;
    void handle_msg(std::shared_ptr<::normal_msg>& msg) override;
    void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) override;
    void peer_close() override;
    void reset() override;
    void destroy() override;

    // 工厂方法（供 UnifiedProtocolFactory 使用）
    static std::unique_ptr<::base_data_process> create(
        std::shared_ptr<base_net_obj> conn,
        IProtocolHandler* handler);

private:
    // TLV 格式解析：[4字节协议ID][4字节长度][数据...]
    size_t parse_tlv_message(const char* data, size_t len);

    IProtocolHandler* _handler;
    std::shared_ptr<BinaryContextImpl> _context;
    std::string _recv_buffer;
    std::string _current_send;
};

} // namespace myframe

#endif // __BINARY_CONTEXT_ADAPTER_H__
