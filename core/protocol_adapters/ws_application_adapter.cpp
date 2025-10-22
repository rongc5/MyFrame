// MyFrame Unified Protocol Architecture - WebSocket Level 1 Adapter Implementation
#include "ws_application_adapter.h"
#include "../web_socket_process.h"
#include "../common_def.h"
#include "../string_pool.h"

namespace myframe {

// ============================================================================
// WsApplicationAdapter Implementation
// ============================================================================

WsApplicationAdapter::WsApplicationAdapter(
    std::shared_ptr<base_net_obj> conn,
    IApplicationHandler* handler)
    : web_socket_res_process(conn)
    , _handler(handler)
{
    // 创建并设置数据处理层
    set_process(new WsApplicationDataProcess(this, handler));

    PDEBUG("[WsApplicationAdapter] Created");
}

WsApplicationAdapter::~WsApplicationAdapter() {
    PDEBUG("[WsApplicationAdapter] Destroyed");
}

std::unique_ptr<base_data_process> WsApplicationAdapter::create(
    std::shared_ptr<base_net_obj> conn,
    IApplicationHandler* handler)
{
    return std::unique_ptr<base_data_process>(
        new WsApplicationAdapter(conn, handler));
}

// ============================================================================
// WsApplicationDataProcess Implementation
// ============================================================================

WsApplicationDataProcess::WsApplicationDataProcess(
    web_socket_process* process,
    IApplicationHandler* handler)
    : web_socket_data_process(process)
    , _handler(handler)
    , _handshake_done(false)
{
    PDEBUG("[WsApplicationDataProcess] Created");
}

WsApplicationDataProcess::~WsApplicationDataProcess() {
    PDEBUG("[WsApplicationDataProcess] Destroyed");
}

void WsApplicationDataProcess::on_handshake_ok() {
    PDEBUG("[WsApplicationDataProcess] WebSocket handshake completed");
    _handshake_done = true;

    // 通知应用层连接建立
    if (_handler) {
        ConnectionInfo info;
        // TODO: 填充连接信息（IP, port 等）
        _handler->on_connect(info);
    }
}

void WsApplicationDataProcess::msg_recv_finish() {
    PDEBUG("[WsApplicationDataProcess] msg_recv_finish called");

    if (!_handshake_done) {
        PDEBUG("[WsApplicationDataProcess] Handshake not completed yet");
        return;
    }

    if (!_handler) {
        PDEBUG("[WsApplicationDataProcess] Handler is null");
        return;
    }

    // 获取接收到的消息
    auto& frame_header = _process->get_recent_recv_frame_header();

    // 构造 WsFrame
    WsFrame recv_frame;
    recv_frame.payload = _recent_msg;
    recv_frame.fin = (frame_header._more_flag == 1);

    // 转换 opcode
    switch (frame_header._op_code) {
        case 0x0: recv_frame.opcode = WsFrame::CONTINUATION; break;
        case 0x1: recv_frame.opcode = WsFrame::TEXT; break;
        case 0x2: recv_frame.opcode = WsFrame::BINARY; break;
        case 0x8: recv_frame.opcode = WsFrame::CLOSE; break;
        case 0x9: recv_frame.opcode = WsFrame::PING; break;
        case 0xA: recv_frame.opcode = WsFrame::PONG; break;
        default:  recv_frame.opcode = WsFrame::TEXT; break;
    }

    PDEBUG("[WsApplicationDataProcess] Received frame: opcode=%d, payload_size=%zu, fin=%d",
           frame_header._op_code, recv_frame.payload.size(), recv_frame.fin);

    try {
        // 准备响应帧
        WsFrame send_frame;

        // 调用用户处理器
        _handler->on_ws(recv_frame, send_frame);

        // 发送响应
        if (!send_frame.payload.empty()) {
            PDEBUG("[WsApplicationDataProcess] Sending response: opcode=%d, payload_size=%zu",
                   (int)send_frame.opcode, send_frame.payload.size());

            // 创建 ws_msg_type
            ws_msg_type msg;
            msg._p_msg = myframe::string_acquire();
            *msg._p_msg = send_frame.payload;

            // 转换 opcode
            switch (send_frame.opcode) {
                case WsFrame::TEXT:         msg._con_type = 0x1; break;
                case WsFrame::BINARY:       msg._con_type = 0x2; break;
                case WsFrame::CLOSE:        msg._con_type = 0x8; break;
                case WsFrame::PING:         msg._con_type = 0x9; break;
                case WsFrame::PONG:         msg._con_type = 0xA; break;
                case WsFrame::CONTINUATION: msg._con_type = 0x0; break;
                default:                    msg._con_type = 0x1; break;
            }

            put_send_msg(msg);
            _process->notice_send();
        }

    } catch (const std::exception& e) {
        PDEBUG("[WsApplicationDataProcess] Exception: %s", e.what());

        // 发生异常，发送错误关闭帧
        ws_msg_type close_msg;
        close_msg._p_msg = myframe::string_acquire();
        *close_msg._p_msg = "Internal Server Error";
        close_msg._con_type = 0x8; // CLOSE
        put_send_msg(close_msg);
        _process->notice_send();
    }
}

void WsApplicationDataProcess::on_close() {
    PDEBUG("[WsApplicationDataProcess] Connection closing");

    if (_handler) {
        _handler->on_disconnect();
    }
}

void WsApplicationDataProcess::on_ping(const char op_code, const std::string& ping_data) {
    PDEBUG("[WsApplicationDataProcess] Received ping, opcode=0x%02x", (unsigned char)op_code);

    // 自动回复 pong
    _process->send_ping(0xA, ping_data);
}

} // namespace myframe
