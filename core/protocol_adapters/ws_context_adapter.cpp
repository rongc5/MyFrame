// MyFrame Unified Protocol Architecture - WebSocket Context Adapter Implementation
#include "ws_context_adapter.h"
#include "../base_net_obj.h"
#include "../common_def.h"
#include "../web_socket_msg.h"
#include "../string_pool.h"
#include "../common_obj_container.h"
#include "../base_net_thread.h"
#include <algorithm>
#include <limits>
#include <thread>

namespace myframe {

// ============================================================================
// WsContextImpl Implementation
// ============================================================================

WsContextImpl::WsContextImpl(web_socket_process* process, std::shared_ptr<base_net_obj> conn)
    : _process(process), _conn(conn), _is_closing(false) {
}

WsContextImpl::~WsContextImpl() {
    _user_data.clear();
}

const WsFrame& WsContextImpl::frame() const {
    return _frame;
}

void WsContextImpl::send_text(const std::string& text) {
    if (_process) {
        auto* data_process = _process->_p_data_process;
        if (data_process) {
            ws_msg_type msg;
            msg._p_msg = string_acquire();
            *msg._p_msg = text;
            msg._con_type = 0x1; // TEXT
            data_process->put_send_msg(msg);
            _process->notice_send();
        }
    }
}

void WsContextImpl::send_binary(const void* data, size_t len) {
    if (_process) {
        auto* data_process = _process->_p_data_process;
        if (data_process) {
            ws_msg_type msg;
            msg._p_msg = string_acquire();
            msg._p_msg->assign(static_cast<const char*>(data), len);
            msg._con_type = 0x2; // BINARY
            data_process->put_send_msg(msg);
            _process->notice_send();
        }
    }
}

void WsContextImpl::send_ping() {
    if (_process) {
        _process->send_ping(0x9, "");
    }
}

void WsContextImpl::send_pong() {
    if (_process) {
        _process->send_ping(0xA, "");
    }
}

void WsContextImpl::close(uint16_t code, const std::string& reason) {
    _is_closing = true;
    if (!_process) return;

    // Build the close payload: 2-byte network-order status code + optional
    // reason. Pass code == 0 to send an empty close payload (no status code).
    // Route through send_close (control-frame queue) so we never go through the
    // data send path, which silently drops empty payloads via the len > 0 gate.
    std::string payload;
    if (code != 0) {
        payload.reserve(2 + reason.size());
        payload.push_back(static_cast<char>((code >> 8) & 0xFF));
        payload.push_back(static_cast<char>(code & 0xFF));
        payload.append(reason);
    }
    _process->send_close(payload);
}

bool WsContextImpl::is_closing() const {
    return _is_closing;
}

size_t WsContextImpl::pending_frames() const {
    return _pending_frames.size();
}

void WsContextImpl::broadcast(const std::string& /* message */) {
    // Not yet implemented - requires a ws_push_hub for connection tracking
    PDEBUG("[WsContext] WARNING: broadcast() called but not supported; ignoring");
}

void WsContextImpl::broadcast_except_self(const std::string& /* message */) {
    // Not yet implemented - requires a ws_push_hub for connection tracking
    PDEBUG("[WsContext] WARNING: broadcast_except_self() called but not supported; ignoring");
}

void WsContextImpl::set_user_data(const std::string& key, void* data) {
    _user_data[key] = data;
}

void* WsContextImpl::get_user_data(const std::string& key) const {
    auto it = _user_data.find(key);
    return (it != _user_data.end()) ? it->second : nullptr;
}

std::shared_ptr<base_net_obj> WsContextImpl::raw_connection() {
    return _conn;
}

::base_net_thread* WsContextImpl::get_thread() const {
    if (_conn) {
        auto* container = _conn->get_net_container();
        if (container) {
            return container->get_owner_thread();
        }
    }
    return nullptr;
}

// ============================================================================
// WsContextAdapter Implementation
// ============================================================================

WsContextAdapter::WsContextAdapter(
    std::shared_ptr<base_net_obj> conn,
    IProtocolHandler* handler)
    : web_socket_res_process(conn)
    , _handler(handler)
{
    set_process(new WsContextDataProcess(this, handler));
    PDEBUG("[WsContextAdapter] Created");
}

WsContextAdapter::~WsContextAdapter() {
    PDEBUG("[WsContextAdapter] Destroyed");
}

std::unique_ptr<::base_data_process> WsContextAdapter::create(
    std::shared_ptr<base_net_obj> conn,
    IProtocolHandler* handler)
{
    return std::unique_ptr<::base_data_process>(
        new WsContextAdapter(conn, handler));
}

// ============================================================================
// WsContextDataProcess Implementation
// ============================================================================

WsContextDataProcess::WsContextDataProcess(
    web_socket_process* process,
    IProtocolHandler* handler)
    : web_socket_data_process(process)
    , _handler(handler)
{
    _context = std::make_shared<WsContextImpl>(process, process->get_base_net());
    PDEBUG("[WsContextDataProcess] Created");
}

WsContextDataProcess::~WsContextDataProcess() {
    PDEBUG("[WsContextDataProcess] Destroyed");
}

void WsContextDataProcess::on_handshake_ok() {
    PDEBUG("[WsContextDataProcess] WebSocket handshake completed");
    _recent_msg.clear();
    on_connect();
}

void WsContextDataProcess::msg_recv_finish() {
    PDEBUG("[WsContextDataProcess] msg_recv_finish called");

    if (!_handler || !_context) return;

    // 获取帧头信息
    auto& frame_header = _process->get_recent_recv_frame_header();

    // 构造 WsFrame
    WsFrame frame;
    frame.payload = _recent_msg;
    frame.fin = (frame_header._completed_fin != 0);

    // 转换 opcode (use _completed_op_code; _op_code is reset by clear() before us)
    int8_t op = frame_header._completed_op_code;
    switch (op) {
        case 0x0: frame.opcode = WsFrame::CONTINUATION; break;
        case 0x1: frame.opcode = WsFrame::TEXT; break;
        case 0x2: frame.opcode = WsFrame::BINARY; break;
        case 0x8: frame.opcode = WsFrame::CLOSE; break;
        case 0x9: frame.opcode = WsFrame::PING; break;
        case 0xA: frame.opcode = WsFrame::PONG; break;
        default:  frame.opcode = WsFrame::TEXT; break;
    }

    PDEBUG("[WsContextDataProcess] Received frame: opcode=%d, payload_size=%zu, fin=%d",
           op, frame.payload.size(), frame.fin);

    // 更新 context
    _context->set_frame(frame);

    // 调用用户处理器
    {
        detail::HandlerContextScope scope(this);
        _handler->on_ws_frame(*_context);
    }

    // Clear the receive buffer so the next frame doesn't accumulate.
    // Without this, msg_recv_finish appends each frame to the same string.
    _recent_msg.clear();
}

void WsContextDataProcess::on_connect() {
    PDEBUG("[WsContextDataProcess] on_connect called");

    if (_handler) {
        detail::HandlerContextScope scope(this);
        ConnectionInfo info;
        // NOTE: ConnectionInfo fields (remote_ip, remote_port) not yet populated
        // from base_net_obj. Callers should not rely on these values.
        _handler->on_connect(info);
    }
}

void WsContextDataProcess::on_close() {
    PDEBUG("[WsContextDataProcess] on_close called");

    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->on_disconnect();
    }
}

void WsContextImpl::add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) {
    if (_conn && t_msg) {
        if (timeout_ms > 0) {
            uint64_t clamped = std::min<uint64_t>(timeout_ms, std::numeric_limits<uint32_t>::max());
            t_msg->_time_length = static_cast<uint32_t>(clamped);
        }
        _conn->add_timer(t_msg);
    }
}

void WsContextImpl::send_msg(std::shared_ptr<::normal_msg> msg) {
    if (_conn && msg) {
        ObjId target = _conn->get_id();
        base_net_thread::put_obj_msg(target, msg);
    }
}

} // namespace myframe
