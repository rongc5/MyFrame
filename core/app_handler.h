#ifndef __APP_HANDLER_H__
#define __APP_HANDLER_H__

#include "common_def.h"
#include <string>
#include <map>
#include <cstddef>
#include <cstdint>

// Forward declarations
struct HttpRequest;
struct HttpResponse;
struct WsFrame;

// 业务层抽象接口 - 完全协议无关
class IAppHandler {
public:
    virtual void on_http(const HttpRequest& req, HttpResponse& rsp) = 0;
    virtual void on_ws(const WsFrame& recv, WsFrame& send) = 0;
    virtual void on_connect() {} // 连接建立回调
    virtual void on_disconnect() {} // 连接断开回调

    // 通用协议回调：用于自定义/二进制等非HTTP/WS协议
    // protocol: 业务自定义协议编号（无编号可传0）
    // data/len: 原始负载字节
    virtual void on_custom(uint32_t /*protocol*/, const char* /*data*/, std::size_t /*len*/) {}

    // 便捷重载：字符串承载二进制亦可
    virtual void on_custom(uint32_t protocol, const std::string& data) {
        on_custom(protocol, data.data(), data.size());
    }
    virtual ~IAppHandler() = default;
};

// HTTP 请求对象
struct HttpRequest {
    std::string method;          // GET, POST, etc.
    std::string url;             // /path?query
    std::string version;         // HTTP/1.1
    std::map<std::string, std::string> headers;
    std::string body;
    
    // 便捷方法
    std::string get_header(const std::string& name) const {
        auto it = headers.find(name);
        return it != headers.end() ? it->second : "";
    }
};

// HTTP 响应对象
struct HttpResponse {
    int status = 200;            // HTTP status code
    std::string reason = "OK";   // Status reason
    std::map<std::string, std::string> headers;
    std::string body;
    
    // 便捷方法
    void set_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }
    
    void set_content_type(const std::string& type) {
        headers["Content-Type"] = type;
    }
};

// WebSocket 帧对象
struct WsFrame {
    enum OpCode {
        TEXT = 0x1,
        BINARY = 0x2,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA
    };
    
    OpCode opcode = TEXT;
    std::string payload;
    bool fin = true;             // Final fragment
    
    // 便捷构造
    static WsFrame text(const std::string& data) {
        WsFrame frame;
        frame.opcode = TEXT;
        frame.payload = data;
        return frame;
    }
    
    static WsFrame binary(const std::string& data) {
        WsFrame frame;
        frame.opcode = BINARY;
        frame.payload = data;
        return frame;
    }
};

#endif
