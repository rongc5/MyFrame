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
    if (_process) {
        auto* data_process = _process->_p_data_process;
        if (data_process) {
            ws_msg_type msg;
            msg._p_msg = string_acquire();
            *msg._p_msg = reason;
            msg._con_type = 0x8; // CLOSE
            data_process->put_send_msg(msg);
            _process->notice_send();
        }
    }
}

bool WsContextImpl::is_closing() const {
    return _is_closing;
}

size_t WsContextImpl::pending_frames() const {
    return _pending_frames.size();
}

void WsContextImpl::broadcast(const std::string& message) {
    // TODO: Implement broadcast functionality
    PDEBUG("[WsContext] broadcast not implemented yet");
}

void WsContextImpl::broadcast_except_self(const std::string& message) {
    // TODO: Implement broadcast_except_self functionality
    PDEBUG("[WsContext] broadcast_except_self not implemented yet");
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
            return ::base_net_thread::get_base_net_thread_obj(container->get_thread_index());
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

void WsContextDataProcess::msg_recv_finish() {
    PDEBUG("[WsContextDataProcess] msg_recv_finish called");

    if (!_handler || !_context) return;

    // 获取帧头信息
    auto& frame_header = _process->get_recent_recv_frame_header();

    // 构造 WsFrame
    WsFrame frame;
    frame.payload = _recent_msg;
    frame.fin = (frame_header._more_flag == 1);

    // 转换 opcode
    switch (frame_header._op_code) {
        case 0x0: frame.opcode = WsFrame::CONTINUATION; break;
        case 0x1: frame.opcode = WsFrame::TEXT; break;
        case 0x2: frame.opcode = WsFrame::BINARY; break;
        case 0x8: frame.opcode = WsFrame::CLOSE; break;
        case 0x9: frame.opcode = WsFrame::PING; break;
        case 0xA: frame.opcode = WsFrame::PONG; break;
        default:  frame.opcode = WsFrame::TEXT; break;
    }

    PDEBUG("[WsContextDataProcess] Received frame: opcode=%d, payload_size=%zu, fin=%d",
           frame_header._op_code, frame.payload.size(), frame.fin);

    // 更新 context
    _context->set_frame(frame);

    // 调用用户处理器
    detail::HandlerContextScope scope(this);
    _handler->on_ws_frame(*_context);
}

void WsContextDataProcess::on_connect() {
    PDEBUG("[WsContextDataProcess] on_connect called");

    if (_handler) {
        detail::HandlerContextScope scope(this);
        ConnectionInfo info;
        // TODO: Fill connection info from base_net_obj
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
