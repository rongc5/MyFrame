# MyFrame 统一协议框架 - 完整实现指南

**更新时间**: 2025-10-22  
**状态**: 实现完成 ?

---

## 目录

1. [概述](#概述)
2. [支持的协议](#支持的协议)
3. [三层架构](#三层架构)
4. [快速开始](#快速开始)
5. [详细使用](#详细使用)
6. [高级特性](#高级特性)
7. [协议优先级](#协议优先级)
8. [常见问题](#常见问题)

---

## 概述

MyFrame 统一协议框架是一个高效的多协议网络服务器框架，支持：

- **文本协议**: HTTP/1.1, HTTP/2, WebSocket (WS)
- **加密协议**: HTTPS, WSS (WebSocket Secure)
- **二进制协议**: 自定义 TLV 格式二进制协议
- **多协议共存**: 在同一端口支持多种协议的自动检测和分发

### 核心特性

? **三层架构** - 从简单到复杂的渐进式增强  
? **协议自动检测** - 无需预先知道协议类型  
? **异步非阻塞** - 基于 epoll 的高性能 I/O  
? **TLS/SSL 支持** - HTTP 升级到 HTTPS, WS 升级到 WSS  
? **HTTP/2 支持** - 通过 ALPN 自动协商  
? **多线程模型** - Listen 线程 + Worker 线程池  

---

## 支持的协议

| 协议 | 端口示例 | 加密 | 检测方式 | 推荐用途 |
|------|---------|------|---------|---------|
| HTTP | 8080 | ? | HTTP 方法 (GET/POST/etc) | RESTful API, 网页 |
| HTTPS | 8443 | ? TLS | TLS Handshake (0x16 0x03) | 安全 RESTful, 网页 |
| HTTP/2 | 8443 | ? TLS | h2 Preface | 高性能 HTTP |
| WebSocket | 8080 | ? | Upgrade 头 + Key | 实时通信 |
| WSS | 8443 | ? TLS | TLS + Upgrade 头 | 安全实时通信 |
| Binary | 自定义 | ? | 协议 ID + 长度 | 高效二进制协议 |

### 协议检测顺序（优先级递减）

```
1. TLS Handshake (0x16 0x03...)  → 进入 TLS 协商
   ├─ TLS 后检测:
   │  ├─ WebSocket Upgrade → WSS
   │  ├─ HTTP/2 Preface → HTTP/2
   │  └─ HTTP Method → HTTPS
   │
2. WebSocket Upgrade → WS
3. HTTP Method → HTTP
4. 自定义二进制协议 → Binary
```

---

## 三层架构

### 架构概览

```
┌────────────────────────────────────────┐
│  Level 1: Application Handler          │
│  简单、协议无关的业务逻辑              │
│  80% 使用场景                          │
└────────────┬───────────────────────────┘
             │ 需要协议细节
┌────────────▼───────────────────────────┐
│  Level 2: Protocol Context Handler     │
│  提供协议细节访问和控制                │
│  异步、流式、状态管理                  │
│  15% 使用场景                          │
└────────────┬───────────────────────────┘
             │ 需要完全控制
┌────────────▼───────────────────────────┐
│  Level 3: Custom Data Process          │
│  完全控制字节流处理                    │
│ 自定义协议和状态机                     │
│ 5% 使用场景（游戏、IoT 等）           │
└────────────────────────────────────────┘
```

### Level 1 - 应用处理器（最简单）

```cpp
class MyHandler : public IApplicationHandler {
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        // 处理 HTTP 请求
        res.set_json(R"({"status":"ok"})");
    }

    void on_ws(const WsFrame& recv, WsFrame& send) override {
        // 处理 WebSocket 消息
        send = WsFrame::text("Echo: " + recv.payload);
    }

    void on_connect(const ConnectionInfo& info) override {
        // 连接建立
    }

    void on_disconnect() override {
        // 连接断开
    }
};
```

### Level 2 - 协议上下文处理器（中等复杂）

```cpp
class AdvancedHandler : public IProtocolHandler {
    void on_http_request(HttpContext& ctx) override {
        const auto& req = ctx.request();
        auto& res = ctx.response();

        // 异步响应
        ctx.async_response([&ctx]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ctx.response().set_json(R"({"data":"async result"})");
        });

        // 流式响应
        ctx.enable_streaming();
        ctx.stream_write("chunk1", 6);
        ctx.stream_write("chunk2", 6);

        // 连接控制
        ctx.keep_alive(true);
        ctx.set_timeout(30000);  // 30 秒超时

        // 用户数据
        ctx.set_user_data("session_id", new std::string("abc123"));
    }

    void on_ws_frame(WsContext& ctx) override {
        const auto& frame = ctx.frame();

        // 状态管理
        int* count = static_cast<int*>(ctx.get_user_data("msg_count"));
        if (!count) {
            count = new int(0);
            ctx.set_user_data("msg_count", count);
        }
        (*count)++;

        // 发送控制
        ctx.send_text("Message #" + std::to_string(*count));
        ctx.send_ping();
        ctx.close(1000, "Goodbye");

        // 查询状态
        if (!ctx.is_closing()) {
            // 处理消息
        }
    }
};
```

### Level 3 - 自定义数据处理（完全控制）

```cpp
class GameProtocol : public base_data_process {
public:
    static bool can_handle(const char* buf, size_t len) {
        return len >= 4 && memcmp(buf, "GAME", 4) == 0;
    }

    static std::unique_ptr<base_data_process> create(
        std::shared_ptr<base_net_obj> conn) {
        return std::make_unique<GameProtocol>(conn);
    }

    size_t process_recv_buf(const char* buf, size_t len) override {
        // 完全自定义的字节流处理
        _buffer.append(buf, len);
        // ... 自定义协议解析 ...
        return len;
    }
};
```

---

## 快速开始

### 1. 最简单的 HTTP 服务器 (Level 1)

```cpp
#include "server.h"
#include "unified_protocol_factory.h"

using namespace myframe;

class SimpleHandler : public IApplicationHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        res.set_json(R"({"message":"Hello World"})");
    }
};

int main() {
    server srv(2);
    auto factory = std::make_shared<UnifiedProtocolFactory>();
    auto handler = std::make_shared<SimpleHandler>();

    factory->register_http_handler(handler.get());

    srv.bind("0.0.0.0", 8080);
    srv.set_business_factory(factory);
    srv.start();
    srv.join();
    return 0;
}
```

编译和运行:
```bash
cd build && cmake .. && make
./examples/unified_simple_http 8080

# 测试
curl http://127.0.0.1:8080/
```

### 2. HTTP + WebSocket 混合服务器

```cpp
class MixedHandler : public IApplicationHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        if (req.url == "/") {
            res.set_html("<h1>Welcome</h1>");
        }
    }

    void on_ws(const WsFrame& recv, WsFrame& send) override {
        send = WsFrame::text("Echo: " + recv.payload);
    }
};

int main() {
    server srv(2);
    auto factory = std::make_shared<UnifiedProtocolFactory>();
    auto handler = std::make_shared<MixedHandler>();

    factory->register_http_handler(handler.get())
           .register_ws_handler(handler.get());

    srv.bind("0.0.0.0", 8080);
    srv.set_business_factory(factory);
    srv.start();
    srv.join();
    return 0;
}
```

### 3. HTTPS/WSS 服务器

```cpp
#include "ssl_context.h"

int main() {
    // 配置 TLS
    ssl_config conf;
    conf._cert_file = "/path/to/cert.crt";
    conf._key_file = "/path/to/key.key";
    tls_set_server_config(conf);

    // 其余代码同上
    server srv(2);
    auto factory = std::make_shared<UnifiedProtocolFactory>();
    // ...
    srv.bind("0.0.0.0", 443);  // HTTPS 端口
    // ...
}
```

---

## 详细使用

### HTTP 请求/响应

```cpp
void on_http(const HttpRequest& req, HttpResponse& res) override {
    // 请求信息
    std::string method = req.method;      // GET, POST, PUT, DELETE, etc.
    std::string url = req.url;            // /api/users?id=123
    std::string version = req.version;    // HTTP/1.1
    std::string header = req.get_header("Content-Type");
    std::string param = req.query_param("id");
    std::string body = req.body;

    // 设置响应
    res.status = 200;
    res.reason = "OK";
    res.set_header("X-Custom", "value");

    // 便捷方法
    res.set_json(R"({"key":"value"})");
    res.set_html("<h1>Hello</h1>");
    res.set_text("Plain text");
}
```

### WebSocket 消息处理

```cpp
void on_ws(const WsFrame& recv, WsFrame& send) override {
    switch (recv.opcode) {
        case WsFrame::TEXT:
            send = WsFrame::text("Echo: " + recv.payload);
            break;
        case WsFrame::BINARY:
            send = WsFrame::binary(recv.payload);
            break;
        case WsFrame::PING:
            send = WsFrame::pong();
            break;
        case WsFrame::CLOSE:
            // 连接关闭
            break;
    }
}
```

### 二进制协议处理

```cpp
void on_binary(const BinaryRequest& req, BinaryResponse& res) override {
    uint32_t proto_id = req.protocol_id;
    const std::vector<uint8_t>& payload = req.payload;

    // 处理不同协议 ID
    switch (proto_id) {
        case PROTO_QUERY:
            res.set_data(handle_query(payload), handle_query(payload).size());
            break;
        case PROTO_UPDATE:
            res.set_data(handle_update(payload), handle_update(payload).size());
            break;
    }
}
```

---

## 高级特性

### 异步响应 (Level 2)

```cpp
void on_http_request(HttpContext& ctx) override {
    ctx.async_response([&ctx]() {
        // 在线程池中执行（不阻塞网络 I/O）
        auto data = database.query("SELECT ...");
        ctx.response().set_json(data);
    });
}
```

### 流式传输 (Level 2)

```cpp
void on_http_request(HttpContext& ctx) override {
    ctx.enable_streaming();

    // 发送文件内容（分块传输）
    std::ifstream file("large_file.bin");
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer))) {
        ctx.stream_write(buffer, file.gcount());
    }
}
```

### 连接状态管理 (Level 2)

```cpp
void on_ws_frame(WsContext& ctx) override {
    // 存储连接相关的状态
    struct UserSession {
        std::string username;
        int message_count;
        std::time_t connect_time;
    };

    auto* session = static_cast<UserSession*>(ctx.get_user_data("session"));
    if (!session) {
        session = new UserSession{"user123", 0, std::time(nullptr)};
        ctx.set_user_data("session", session);
    }

    session->message_count++;
    std::cout << "Message count: " << session->message_count << std::endl;
}
```

---

## 协议优先级

**协议检测优先级表**（数字越小优先级越高）

| 优先级 | 协议 | 检测条件 | 说明 |
|--------|------|---------|------|
| 1-9 | 特殊协议 | TLS 握手（0x16 0x03...） | 先进行 TLS 协商 |
| 10 | WebSocket | Upgrade + websocket | 在 TLS 之后优先检测 |
| 20 | HTTP | GET/POST/PUT/DELETE | 常见文本协议 |
| 30-49 | 其他文本 | 自定义 | Redis、SMTP 等 |
| 50-79 | 二进制协议 | TLV 格式 | 自定义二进制协议 |
| 100+ | 兜底协议 | 用户自定义 | 作为最后的备选方案 |

**自定义优先级**:

```cpp
// 注册时指定优先级
factory->register_binary_handler("game_protocol", detect_fn, handler, 55);
factory->register_custom_protocol_class<GameProtocol>("game", 55);
```

---

## 常见问题

### Q1: 如何在同一个端口支持 HTTP 和 HTTPS?

**A**: 框架会自动检测！只需注册处理器：

```cpp
factory->register_http_handler(handler.get());
// TLS 配置
ssl_config conf;
conf._cert_file = "cert.crt";
conf._key_file = "key.key";
tls_set_server_config(conf);
```

如果客户端发送 TLS 握手（0x16 0x03...），框架自动切换到 HTTPS；否则使用 HTTP。

### Q2: 如何支持 WebSocket 和 HTTP2 在同一个连接上?

**A**: 这在 HTTP/1.1 上不支持（协议标准限制），但在 HTTP/2 上原生支持。
框架会自动通过 ALPN 协商 h2 或 http/1.1。

### Q3: 如何实现自定义的二进制协议？

**A**: 创建一个继承 `base_data_process` 的类或使用 `IApplicationHandler::on_binary()`:

```cpp
class MyBinaryHandler : public IApplicationHandler {
public:
    void on_binary(const BinaryRequest& req, BinaryResponse& res) override {
        // 处理协议
    }
};

factory->register_binary_handler("myproto", 
    [](const char* buf, size_t len) {
        return len >= 4 && memcmp(buf, "MYID", 4) == 0;
    },
    handler.get(), 50);
```

### Q4: 如何处理大文件上传?

**A**: 使用 Level 2 的流式处理：

```cpp
void on_http_request(HttpContext& ctx) override {
    if (ctx.request().method == "POST") {
        // 分块处理上传的数据
        // 框架会自动缓冲接收
        std::string body = ctx.request().body;  // 从缓冲中读取
        // 处理...
    }
}
```

### Q5: 如何实现 WebSocket 广播?

**A**: 使用 Level 2 Context:

```cpp
void on_ws_frame(WsContext& ctx) override {
    // 广播给所有连接
    ctx.broadcast(ctx.frame().payload);
    
    // 广播给除自己之外的连接
    ctx.broadcast_except_self(ctx.frame().payload);
}
```

### Q6: 如何监控连接数和吞吐量?

**A**: 利用连接 ID 和生命周期钩子：

```cpp
class StatsHandler : public IApplicationHandler {
    std::atomic<int> connection_count{0};

    void on_connect(const ConnectionInfo& info) override {
        connection_count++;
        std::cout << "Active connections: " << connection_count << std::endl;
    }

    void on_disconnect() override {
        connection_count--;
    }
};
```

---

## 构建和测试

### 编译

```bash
cd /home/rong/myframe/build
cmake ..
make -j$(nproc)
```

### 运行示例

```bash
# 简单 HTTP 服务器
./examples/unified_simple_http 8080

# 混合协议服务器
./examples/unified_mixed_server 8080

# Level 2 高级示例
./examples/unified_level2_demo 8080

# HTTPS/WSS 服务器
./examples/unified_https_wss_server 8443

# 测试
curl http://127.0.0.1:8080/
curl -k https://127.0.0.1:8443/
```

---

## 下一步

- ? 查看具体示例代码：`examples/unified_*.cpp`
- ? API 参考：`core/app_handler_v2.h`, `core/protocol_context.h`
- ? 自定义协议：继承 `base_data_process` 或使用 `IApplicationHandler`

**文档版本**: 2.0  
**最后更新**: 2025-10-22  
**框架版本**: MyFrame v2.0 (Unified Protocol Architecture)
