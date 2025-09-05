#pragma once

#include "web_socket_data_process.h"
#include "web_socket_process.h"
#include "app_handler.h"

class app_ws_data_process : public web_socket_data_process {
public:
    app_ws_data_process(web_socket_process* p, IAppHandler* h)
        : web_socket_data_process(p), _handler(h) {}

    virtual void msg_recv_finish() override {
        // 构造接收帧
        WsFrame recv;
        int8_t op = _process->get_recent_recv_frame_header()._op_code;
        if (op == 0x1) recv.opcode = WsFrame::TEXT; else recv.opcode = WsFrame::BINARY;
        recv.payload = _recent_msg;
        _recent_msg.clear();

        // 回调业务
        WsFrame send = WsFrame::text("");
        if (_handler) {
            _handler->on_ws(recv, send);
        }

        // 组装发送帧：header + payload
        std::string header = _process->get_recent_send_frame_header().gen_frame_header(send.payload.size(), std::string(), (int8_t)send.opcode);
        std::string* out = new std::string;
        out->reserve(header.size() + send.payload.size());
        out->append(header);
        out->append(send.payload);

        ws_msg_type m; m.init(); m._p_msg = out; m._con_type = (int8_t)send.opcode;
        put_send_msg(m);
    }

private:
    IAppHandler* _handler;
};

