// MyFrame Unified Protocol Architecture - Level 1 Interface
// Application Handler - Protocol-agnostic business logic
#ifndef __APP_HANDLER_V2_H__
#define __APP_HANDLER_V2_H__

#include "base_def.h"
#include "common_def.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cstddef>
#include <memory>

// Forward declarations (global namespace)
class normal_msg;
class timer_msg;
class base_net_thread;
class base_data_process;

namespace myframe {
namespace detail {

// RAII scope：在 handler 回调过程中记录当前数据流程上下文
class HandlerContextScope {
public:
    explicit HandlerContextScope(::base_data_process* process);
    ~HandlerContextScope();
private:
    ::base_data_process* _previous;
};

// 获取当前线程的 handler 执行上下文（若无则返回 nullptr）
::base_data_process* current_process();

} // namespace detail
} // namespace myframe

namespace myframe {

// ============================================================================
// Level 1: Application Handler (应用层)
// 用户只需实现业务逻辑，不关心协议细节
// ============================================================================

// 连接信息
struct ConnectionInfo {
    std::string remote_ip;
    unsigned short remote_port;
    std::string local_ip;
    unsigned short local_port;
    uint64_t connection_id;

    ConnectionInfo()
        : remote_port(0), local_port(0), connection_id(0) {}
};

// ============================================================================
// HTTP 协议抽象
// ============================================================================

// HTTP 请求对象
struct HttpRequest {
    std::string method;          // GET, POST, PUT, DELETE, etc.
    std::string url;             // /path?query
    std::string version;         // HTTP/1.1, HTTP/2, etc.
    std::map<std::string, std::string> headers;
    std::string body;

    HttpRequest() : method("GET"), version("HTTP/1.1") {}

    // 便捷方法
    std::string get_header(const std::string& name) const {
        auto it = headers.find(name);
        return it != headers.end() ? it->second : std::string();
    }

    // 解析 query 参数（简单实现）
    std::string query_param(const std::string& name) const {
        size_t query_pos = url.find('?');
        if (query_pos == std::string::npos) return std::string();

        std::string query = url.substr(query_pos + 1);
        std::string search = name + "=";
        size_t pos = query.find(search);
        if (pos == std::string::npos) return std::string();

        size_t start = pos + search.length();
        size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.length();

        return query.substr(start, end - start);
    }
};

// HTTP 响应对象
struct HttpResponse {
    int status;
    std::string reason;
    std::map<std::string, std::string> headers;
    std::string body;

    HttpResponse() : status(200), reason("OK") {}

    // 便捷方法
    void set_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }

    void set_content_type(const std::string& type) {
        headers["Content-Type"] = type;
    }

    void set_json(const std::string& json_body) {
        headers["Content-Type"] = "application/json; charset=utf-8";
        body = json_body;
    }

    void set_html(const std::string& html_body) {
        headers["Content-Type"] = "text/html; charset=utf-8";
        body = html_body;
    }

    void set_text(const std::string& text_body) {
        headers["Content-Type"] = "text/plain; charset=utf-8";
        body = text_body;
    }
};

// ============================================================================
// WebSocket 协议抽象
// ============================================================================

// WebSocket 帧对象
struct WsFrame {
    enum OpCode {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA
    };

    OpCode opcode;
    std::string payload;
    bool fin;

    WsFrame() : opcode(TEXT), fin(true) {}

    // 便捷构造方法
    static WsFrame text(const std::string& data) {
        WsFrame frame;
        frame.opcode = TEXT;
        frame.payload = data;
        frame.fin = true;
        return frame;
    }

    static WsFrame binary(const std::string& data) {
        WsFrame frame;
        frame.opcode = BINARY;
        frame.payload = data;
        frame.fin = true;
        return frame;
    }

    static WsFrame binary(const void* data, size_t len) {
        WsFrame frame;
        frame.opcode = BINARY;
        frame.payload.assign(static_cast<const char*>(data), len);
        frame.fin = true;
        return frame;
    }

    static WsFrame ping() {
        WsFrame frame;
        frame.opcode = PING;
        frame.fin = true;
        return frame;
    }

    static WsFrame pong() {
        WsFrame frame;
        frame.opcode = PONG;
        frame.fin = true;
        return frame;
    }
};

// ============================================================================
// 二进制协议抽象
// ============================================================================

// 二进制请求对象
struct BinaryRequest {
    uint32_t protocol_id;                      // 协议标识
    std::vector<uint8_t> payload;              // 数据负载
    std::map<std::string, std::string> metadata; // 可选元数据

    BinaryRequest() : protocol_id(0) {}
};

// 二进制响应对象
struct BinaryResponse {
    std::vector<uint8_t> data;                 // 响应数据
    std::map<std::string, std::string> metadata; // 可选元数据

    BinaryResponse() {}

    // 便捷方法
    void set_data(const void* ptr, size_t len) {
        data.assign(static_cast<const uint8_t*>(ptr),
                    static_cast<const uint8_t*>(ptr) + len);
    }
};

// ============================================================================
// Level 1 应用处理器接口
// ============================================================================

class IApplicationHandler {
public:
    virtual ~IApplicationHandler() {}

    // HTTP 处理 - 子类可选实现
    virtual void on_http(const HttpRequest& req, HttpResponse& res) {
        res.status = 501;
        res.reason = "Not Implemented";
        res.set_text("HTTP handler not implemented");
    }

    // WebSocket 处理 - 子类可选实现
    virtual void on_ws(const WsFrame& recv, WsFrame& send) {
        send = WsFrame::text("WebSocket handler not implemented");
    }

    // 二进制协议处理 - 子类可选实现
    virtual void on_binary(const BinaryRequest& req, BinaryResponse& res) {
        // 默认不实现
        (void)req;
        (void)res;
    }

    // 生命周期钩子 - 子类可选实现
    virtual void on_connect(const ConnectionInfo& info) {
        (void)info;
    }

    virtual void on_disconnect() {
        // 默认不实现
    }

    virtual void on_error(const std::string& error) {
        (void)error;
    }

    // ========== 消息和超时处理 ==========

    // 处理消息（用于异步操作、跨线程通信等）
    virtual void handle_msg(std::shared_ptr<::normal_msg>& msg) {
        (void)msg;
    }

    virtual bool handle_thread_msg(std::shared_ptr<::normal_msg>& msg) {
        handle_msg(msg);
        return true;
    }

    // 处理超时（用于定时器、超时控制等）
    virtual void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) {
        (void)t_msg;
    }

    // ========== 定时器辅助 ==========
    // 触发一个延迟执行的超时，返回生成的 timer_id（失败时返回 0）
    uint32_t schedule_timeout(uint32_t delay_ms, uint32_t timer_type = APPLICATION_TIMER_TYPE) const;

    // ========== 线程访问 ==========
    // 获取当前业务线程
    // 返回 nullptr 表示线程信息不可用
    virtual ::base_net_thread* get_current_thread() const;

    // ========== 连接标识 ==========
    // 获取当前连接的 ObjId（若不可用则返回 {0,0}）
    ObjId current_connection_id() const;
};

} // namespace myframe

#endif // __APP_HANDLER_V2_H__
