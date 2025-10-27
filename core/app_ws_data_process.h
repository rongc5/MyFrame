#pragma once

#include "web_socket_data_process.h"
#include "web_socket_process.h"
#include "app_handler_v2.h"
#include <string>
#include <algorithm>

// Forward declare hub to avoid circular include
class WsPushHub;

class app_ws_data_process : public web_socket_data_process {
public:
    app_ws_data_process(web_socket_process* p, myframe::IApplicationHandler* h)
        : web_socket_data_process(p), _handler(h) {}

    ~app_ws_data_process() override {
        // 连接销毁时从Hub注销
        unregister_self();
    }

    // 连接关闭回调：即时注销Hub
    void on_close() override {
        unregister_self();
    }

    // 握手完成回调：解析用户名并注册到Hub
    void on_handshake_ok() override {
        web_socket_data_process::on_handshake_ok();
        const std::string &hdr = _process->get_recv_header();
        // 大小写不敏感查找Cookie行，但不改变原始内容
        std::string lower = hdr;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        size_t pos = lower.find("cookie:");
        if (pos != std::string::npos) {
            size_t line_end = hdr.find("\r\n", pos);
            std::string line = hdr.substr(pos, (line_end == std::string::npos ? hdr.size() : line_end) - pos);
            // 提取冒号后的值
            size_t colon = line.find(':');
            std::string value = (colon == std::string::npos) ? std::string() : line.substr(colon + 1);
            // 去除前后空白
            auto ltrim = [](std::string& s){ s.erase(0, s.find_first_not_of(" \t\r\n")); };
            auto rtrim = [](std::string& s){ s.erase(s.find_last_not_of(" \t\r\n") + 1); };
            ltrim(value); rtrim(value);
            // 解析 cookie 键值对，按分号分割
            size_t i = 0;
            while (i < value.size()) {
                size_t semi = value.find(';', i);
                std::string pair = value.substr(i, semi == std::string::npos ? value.size() - i : semi - i);
                if (semi == std::string::npos) i = value.size(); else i = semi + 1;
                ltrim(pair); rtrim(pair);
                size_t eq = pair.find('=');
                if (eq == std::string::npos) continue;
                std::string key = pair.substr(0, eq); ltrim(key); rtrim(key);
                std::string val = pair.substr(eq + 1); ltrim(val); rtrim(val);
                std::string key_lower = key; std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), ::tolower);
                if (key_lower == "username") {
                    size_t bar = val.find('|');
                    _username = (bar != std::string::npos) ? val.substr(0, bar) : val;
                    break;
                }
            }
        }
        register_self();
    }

    // 主动发送文本帧（服务端->客户端非掩码）
    void send_text(const std::string& payload) {
        std::string header = _process->get_recent_send_frame_header().gen_frame_header(payload.size(), std::string(), (int8_t)myframe::WsFrame::TEXT);
        std::string* out = new std::string;
        out->reserve(header.size() + payload.size());
        out->append(header);
        out->append(payload);
        ws_msg_type m; m.init(); m._p_msg = out; m._con_type = (int8_t)myframe::WsFrame::TEXT;
        put_send_msg(m);
        _process->notice_send();
    }

    virtual void msg_recv_finish() override {
        // 构造接收帧
        myframe::WsFrame recv;
        int8_t op = _process->get_recent_recv_frame_header()._op_code;
        if (op == 0x1) recv.opcode = myframe::WsFrame::TEXT; else recv.opcode = myframe::WsFrame::BINARY;
        recv.payload = _recent_msg;
        _recent_msg.clear();

        // 回调业务
        myframe::WsFrame send = myframe::WsFrame::text("");
        if (_handler) {
            myframe::detail::HandlerContextScope scope(this);
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
    void handle_msg(std::shared_ptr<normal_msg>& msg) override {
        if (_handler) {
            myframe::detail::HandlerContextScope scope(this);
            _handler->handle_thread_msg(msg);
        }
    }
    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (_handler) {
            myframe::detail::HandlerContextScope scope(this);
            _handler->handle_timeout(t_msg);
        }
    }

private:
    myframe::IApplicationHandler* _handler;
    std::string _username;

    void register_self();
    void unregister_self();
};

// 内联实现注册/注销，避免引入链接顺序问题
#include "ws_push_hub.h"
inline void app_ws_data_process::register_self() {
    if (!_username.empty()) {
        WsPushHub::Instance().Register(_username, this);
        // 首次建立连接后，推送一次初始化事件，供业务侧（如前端）拉取初始数据
        try { this->send_text(std::string("{\"type\":\"init\"}")); } catch(...) {}
    }
}
inline void app_ws_data_process::unregister_self() {
    WsPushHub::Instance().Unregister(this);
}
