// MyFrame Unified Protocol Architecture - Binary Context Adapter Implementation
#include "binary_context_adapter.h"
#include "../base_net_obj.h"
#include "../common_def.h"
#include "../common_obj_container.h"
#include "../base_net_thread.h"
#include <algorithm>
#include <limits>

namespace myframe {

// ============================================================================
// BinaryContextImpl Implementation
// ============================================================================

BinaryContextImpl::BinaryContextImpl(std::shared_ptr<base_net_obj> conn)
    : _conn(conn), _state(HANDSHAKE), _proto_id(0), _streaming_enabled(false) {
}

BinaryContextImpl::~BinaryContextImpl() {
    _user_data.clear();
}

BinaryContext::State BinaryContextImpl::state() const {
    return _state;
}

void BinaryContextImpl::set_state(BinaryContext::State s) {
    _state = s;
}

uint32_t BinaryContextImpl::protocol_id() const {
    return _proto_id;
}

const uint8_t* BinaryContextImpl::payload() const {
    return reinterpret_cast<const uint8_t*>(_payload.data());
}

size_t BinaryContextImpl::payload_size() const {
    return _payload.size();
}

void BinaryContextImpl::send(uint32_t proto_id, const void* data, size_t len) {
    PendingMessage msg;
    msg.proto_id = proto_id;
    msg.data.assign(static_cast<const char*>(data), len);
    _pending_queue.push(msg);
    if (_conn) {
        _conn->notice_send();
    }
}

void BinaryContextImpl::send_async(std::function<void()> fn) {
    // 简单实现：直接执行，未来可以改为异步队列
    if (fn) {
        fn();
    }
}

void BinaryContextImpl::enable_streaming() {
    _streaming_enabled = true;
}

size_t BinaryContextImpl::stream_write(const void* data, size_t len) {
    if (!_streaming_enabled || !_conn) return 0;

    // TODO: 实现流式写入
    PDEBUG("[BinaryContext] stream_write not fully implemented yet");
    return len;
}

void BinaryContextImpl::set_user_data(const std::string& key, void* data) {
    _user_data[key] = data;
}

void* BinaryContextImpl::get_user_data(const std::string& key) const {
    auto it = _user_data.find(key);
    return (it != _user_data.end()) ? it->second : nullptr;
}

std::shared_ptr<base_net_obj> BinaryContextImpl::raw_connection() {
    return _conn;
}

::base_net_thread* BinaryContextImpl::get_thread() const {
    if (_conn) {
        auto* container = _conn->get_net_container();
        if (container) {
            return ::base_net_thread::get_base_net_thread_obj(container->get_thread_index());
        }
    }
    return nullptr;
}

void BinaryContextImpl::set_message(uint32_t proto_id, const std::string& payload) {
    _proto_id = proto_id;
    _payload = payload;
}

bool BinaryContextImpl::get_pending_message(uint32_t& proto_id, std::string& data) {
    if (_pending_queue.empty()) {
        return false;
    }

    auto msg = _pending_queue.front();
    _pending_queue.pop();
    proto_id = msg.proto_id;
    data = msg.data;
    return true;
}

// ============================================================================
// BinaryContextAdapter Implementation
// ============================================================================

BinaryContextAdapter::BinaryContextAdapter(
    std::shared_ptr<base_net_obj> conn,
    IProtocolHandler* handler)
    : ::base_data_process(conn)
    , _handler(handler)
{
    _context = std::make_shared<BinaryContextImpl>(conn);
    _context->set_state(BinaryContext::READY);

    // 通知连接建立
    if (_handler) {
        ConnectionInfo info;
        // TODO: Fill connection info
        detail::HandlerContextScope scope(this);
        _handler->on_connect(info);
    }

    PDEBUG("[BinaryContextAdapter] Created");
}

BinaryContextAdapter::~BinaryContextAdapter() {
    PDEBUG("[BinaryContextAdapter] Destroyed");
}

std::unique_ptr<::base_data_process> BinaryContextAdapter::create(
    std::shared_ptr<base_net_obj> conn,
    IProtocolHandler* handler)
{
    return std::unique_ptr<::base_data_process>(
        new BinaryContextAdapter(conn, handler));
}

size_t BinaryContextAdapter::process_recv_buf(const char* buf, size_t len) {
    if (!_handler) return len;

    _recv_buffer.append(buf, len);

    size_t offset = 0;
    while (offset < _recv_buffer.size()) {
        size_t available = _recv_buffer.size() - offset;
        size_t parsed = parse_tlv_message(_recv_buffer.data() + offset, available);
        if (parsed == 0) {
            break;
        }
        offset += parsed;
    }

    // 清理已消费的数据
    if (offset > 0) {
        _recv_buffer.erase(0, offset);
    }

    return len;
}

size_t BinaryContextAdapter::parse_tlv_message(const char* data, size_t len) {
    // TLV 格式：[4字节协议ID][4字节长度][数据...]
    const size_t HEADER_SIZE = 8;

    if (len < HEADER_SIZE) {
        return 0;  // 数据不足
    }

    // 解析协议ID和长度
    uint32_t proto_id = 0;
    uint32_t data_len = 0;
    std::memcpy(&proto_id, data, sizeof(uint32_t));
    std::memcpy(&data_len, data + 4, sizeof(uint32_t));

    proto_id = ntohl(proto_id);
    data_len = ntohl(data_len);

    // 检查长度的合理性（防止攻击）
    if (data_len > 10 * 1024 * 1024) {  // 最大10MB
        PDEBUG("[BinaryContextAdapter] Invalid data length: %u", data_len);
        return 0;
    }

    // 检查是否有完整的消息
    if (len < HEADER_SIZE + data_len) {
        return 0;  // 数据不完整
    }

    // 提取数据
    std::string payload(data + HEADER_SIZE, data_len);

    // 更新 context
    _context->set_message(proto_id, payload);

    // 调用用户处理器
    detail::HandlerContextScope scope(this);
    _handler->on_binary_message(*_context);

    PDEBUG("[BinaryContextAdapter] Processed message: proto_id=%u, len=%u", proto_id, data_len);

    return HEADER_SIZE + data_len;
}

std::string* BinaryContextAdapter::get_send_buf() {
    if (!_context->has_pending_send()) {
        return nullptr;
    }

    uint32_t proto_id = 0;
    std::string data;

    if (!_context->get_pending_message(proto_id, data)) {
        return nullptr;
    }

    // 构造 TLV 格式响应
    const uint32_t data_len = static_cast<uint32_t>(data.size());
    uint32_t net_proto_id = htonl(proto_id);
    uint32_t net_data_len = htonl(data_len);
    std::string* frame = new std::string;
    frame->resize(8 + data_len);
    char* raw = frame->empty() ? nullptr : &(*frame)[0];

    std::memcpy(raw, &net_proto_id, sizeof(uint32_t));
    std::memcpy(raw + 4, &net_data_len, sizeof(uint32_t));
    if (data_len > 0) {
        std::memcpy(raw + 8, data.data(), data_len);
    }

    PDEBUG("[BinaryContextAdapter] Sending message: proto_id=%u, len=%u", proto_id, data_len);

    return frame;
}

void BinaryContextAdapter::handle_msg(std::shared_ptr<::normal_msg>& msg) {
    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->handle_msg(msg);
    }
}

void BinaryContextAdapter::handle_timeout(std::shared_ptr<::timer_msg>& t_msg) {
    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->handle_timeout(t_msg);
    }
}

void BinaryContextAdapter::peer_close() {
    PDEBUG("[BinaryContextAdapter] Connection closing");

    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->on_disconnect();
    }
}

void BinaryContextAdapter::reset() {
    _recv_buffer.clear();
}

void BinaryContextAdapter::destroy() {
    reset();
}

void BinaryContextImpl::add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) {
    if (_conn && t_msg) {
        if (timeout_ms > 0) {
            uint64_t clamped = std::min<uint64_t>(timeout_ms, std::numeric_limits<uint32_t>::max());
            t_msg->_time_length = static_cast<uint32_t>(clamped);
        }
        _conn->add_timer(t_msg);
    }
}

void BinaryContextImpl::send_msg(std::shared_ptr<::normal_msg> msg) {
    if (_conn && msg) {
        ObjId target = _conn->get_id();
        base_net_thread::put_obj_msg(target, msg);
    }
}

} // namespace myframe
