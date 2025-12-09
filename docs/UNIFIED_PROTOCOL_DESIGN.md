# MyFrame 统一协议处理架构设计

## 1. 设计目标

### 1.1 核心原则
- **默认简单，按需复杂** - 90%的场景用简单API，10%需要深度控制
- **协议和业务分离** - 协议解析由框架负责，业务只关心逻辑
- **渐进式增强** - 从Level 1到Level 3逐步下沉
- **统一注册机制** - 所有协议都通过统一方式注册和检测

### 1.2 替代现有设计
- 废弃: `MultiProtocolFactory + IAppHandler` (过于简化，灵活性不足)
- 保留: 旧框架的 `base_data_process` 继承方式（作为 Level 3）
- 新增: Level 1 和 Level 2 抽象层

---

## 2. 三层架构设计

- Level 1: Application Handler（应用层），提供协议无关的请求/响应模型，适合简单 CRUD、RESTful API、轻量 RPC。
- Level 2: Protocol Context Handler（协议层），可访问协议细节和状态，支持异步、流式和有状态交互，适合 WebSocket 实时通信、复杂 RPC。
- Level 3: Data Process（传输层），直接控制字节流和协议解析，适合游戏协议、自定义二进制协议、IoT 等需要完全掌控的场景。

---

## 3. Level 1: Application Handler (应用层)

### 3.1 设计理念
- 用户只需实现业务逻辑，不关心协议细节
- 框架提供标准的请求/响应对象
- 适合90%的常规业务场景

### 3.2 核心接口

```cpp
// core/app_handler.h
namespace myframe {

// HTTP 请求对象
struct HttpRequest {
    std::string method;          // GET, POST, etc.
    std::string url;             // /path?query
    std::string version;         // HTTP/1.1
    std::map<std::string, std::string> headers;
    std::string body;

    std::string get_header(const std::string& name) const;
    std::string query_param(const std::string& name) const;
};

// HTTP 响应对象
struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    void set_header(const std::string& name, const std::string& value);
    void set_content_type(const std::string& type);
    void set_json(const std::string& json);
};

// WebSocket 帧对象
struct WsFrame {
    enum OpCode { TEXT = 0x1, BINARY = 0x2, CLOSE = 0x8, PING = 0x9, PONG = 0xA };

    OpCode opcode = TEXT;
    std::string payload;
    bool fin = true;

    static WsFrame text(const std::string& data);
    static WsFrame binary(const std::string& data);
};

// 二进制协议请求/响应
struct BinaryRequest {
    uint32_t protocol_id;        // 协议标识
    std::vector<uint8_t> payload;
    std::map<std::string, std::string> metadata;  // 可选元数据
};

struct BinaryResponse {
    std::vector<uint8_t> data;
    std::map<std::string, std::string> metadata;
};

// 连接信息
struct ConnectionInfo {
    std::string remote_ip;
    unsigned short remote_port;
    std::string local_ip;
    unsigned short local_port;
    uint64_t connection_id;
};

// Level 1 抽象接口
class IApplicationHandler {
public:
    virtual ~IApplicationHandler() = default;

    // HTTP 处理
    virtual void on_http(const HttpRequest& req, HttpResponse& res) {
        res.status = 404;
        res.body = "Not Implemented";
    }

    // WebSocket 处理
    virtual void on_ws(const WsFrame& recv, WsFrame& send) {
        send = WsFrame::text("echo: " + recv.payload);
    }

    // 二进制协议处理
    virtual void on_binary(const BinaryRequest& req, BinaryResponse& res) {
        // 默认不实现
    }

    // 生命周期钩子
    virtual void on_connect(const ConnectionInfo& info) {}
    virtual void on_disconnect() {}
    virtual void on_error(const std::string& error) {}
};

} // namespace myframe
```

### 3.3 使用示例

```cpp
// 示例1: 简单 HTTP API
class MyHttpHandler : public IApplicationHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        if (req.url == "/api/hello") {
            res.set_json("{\"message\":\"Hello World\"}");
        } else if (req.url == "/api/user") {
            handle_user_api(req, res);
        } else {
            res.status = 404;
            res.body = "Not Found";
        }
    }

private:
    void handle_user_api(const HttpRequest& req, HttpResponse& res) {
        std::string user_id = req.query_param("id");
        auto user_data = db_.get_user(user_id);
        res.set_json(user_data.to_json());
    }

    Database db_;
};

// 示例2: WebSocket 聊天
class ChatHandler : public IApplicationHandler {
public:
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        if (recv.payload == "ping") {
            send = WsFrame::text("pong");
        } else {
            // 广播消息
            broadcast(recv.payload);
        }
    }

    void on_connect(const ConnectionInfo& info) override {
        LOG_INFO("New chat user from %s:%d", info.remote_ip.c_str(), info.remote_port);
    }
};

// 示例3: 简单二进制协议
class MyBinaryHandler : public IApplicationHandler {
public:
    void on_binary(const BinaryRequest& req, BinaryResponse& res) override {
        switch (req.protocol_id) {
            case PROTO_QUERY:
                res.data = handle_query(req.payload);
                break;
            case PROTO_UPDATE:
                res.data = handle_update(req.payload);
                break;
        }
    }
};
```

---

## 4. Level 2: Protocol Context Handler (协议层)

### 4.1 设计理念
- 提供协议状态访问和控制能力
- 支持异步响应、流式处理
- 适合需要深度协议交互的场景

### 4.2 核心接口

```cpp
// core/protocol_context.h
namespace myframe {

// HTTP 协议上下文
class HttpContext {
public:
    // 请求访问
    const HttpRequest& request() const;
    HttpResponse& response();

    // 高级控制
    void async_response(std::function<void()> fn);     // 异步响应
    void enable_streaming();                            // 流式响应
    size_t stream_write(const void* data, size_t len); // 流式写入
    void upgrade_to_websocket();                        // 协议升级

    // 连接控制
    void close();
    void set_timeout(uint64_t ms);
    void keep_alive(bool enable);

    // 用户数据
    void set_user_data(const std::string& key, void* data);
    void* get_user_data(const std::string& key) const;

    // 底层访问
    ConnectionInfo& connection_info();
    base_net_obj* raw_connection();
};

// WebSocket 协议上下文
class WsContext {
public:
    // 帧访问
    const WsFrame& frame() const;

    // 发送控制
    void send_text(const std::string& text);
    void send_binary(const void* data, size_t len);
    void send_ping();
    void send_pong();
    void close(uint16_t code = 1000, const std::string& reason = "");

    // 状态查询
    bool is_closing() const;
    size_t pending_frames() const;

    // 广播支持（需要配合 Hub）
    void broadcast(const std::string& message);
    void broadcast_except_self(const std::string& message);

    // 用户数据
    void set_user_data(const std::string& key, void* data);
    void* get_user_data(const std::string& key) const;
};

// 二进制协议上下文
class BinaryContext {
public:
    enum State { HANDSHAKE, READY, STREAMING, CLOSING };

    // 状态控制
    State state() const;
    void set_state(State s);

    // 消息访问
    uint32_t protocol_id() const;
    const uint8_t* payload() const;
    size_t payload_size() const;

    // 响应控制
    void send(uint32_t proto_id, const void* data, size_t len);
    void send_async(std::function<void()> fn);

    // 流式处理
    void enable_streaming();
    size_t stream_write(const void* data, size_t len);

    // 用户数据
    void set_user_data(const std::string& key, void* data);
    void* get_user_data(const std::string& key) const;

    // 底层访问
    base_net_obj* raw_connection();
};

// Level 2 抽象接口
class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    virtual void on_http_request(HttpContext& ctx) {}
    virtual void on_ws_frame(WsContext& ctx) {}
    virtual void on_binary_message(BinaryContext& ctx) {}

    virtual void on_connect(ConnectionInfo& info) {}
    virtual void on_disconnect() {}
};

} // namespace myframe
```

### 4.3 使用示例

```cpp
// 示例1: 异步 HTTP 处理
class AsyncHttpHandler : public IProtocolHandler {
public:
    void on_http_request(HttpContext& ctx) override {
        auto& req = ctx.request();

        if (req.url == "/api/slow") {
            // 异步处理长时间任务
            ctx.async_response([&ctx, this]() {
                auto data = fetch_from_database();  // 耗时操作
                ctx.response().set_json(data);
            });
        } else {
            ctx.response().body = "Fast response";
        }
    }
};

// 示例2: WebSocket 带状态管理
class WsAuthHandler : public IProtocolHandler {
public:
    void on_ws_frame(WsContext& ctx) override {
        bool* authenticated = static_cast<bool*>(
            ctx.get_user_data("authenticated"));

        if (!authenticated || !*authenticated) {
            // 未认证，要求登录
            auto& frame = ctx.frame();
            if (frame.payload.find("auth:") == 0) {
                std::string token = frame.payload.substr(5);
                if (validate_token(token)) {
                    bool* auth = new bool(true);
                    ctx.set_user_data("authenticated", auth);
                    ctx.send_text("auth_ok");
                } else {
                    ctx.close(1008, "Authentication failed");
                }
            }
        } else {
            // 已认证，处理业务
            handle_business_message(ctx);
        }
    }

    void on_disconnect() override {
        // 清理资源
    }
};

// 示例3: 二进制 RPC 协议
class RpcProtocolHandler : public IProtocolHandler {
public:
    void on_binary_message(BinaryContext& ctx) override {
        switch (ctx.state()) {
            case BinaryContext::HANDSHAKE:
                handle_handshake(ctx);
                break;

            case BinaryContext::READY:
                handle_rpc_call(ctx);
                break;

            case BinaryContext::STREAMING:
                handle_streaming_data(ctx);
                break;
        }
    }

private:
    void handle_handshake(BinaryContext& ctx) {
        // 验证握手
        if (validate_handshake(ctx.payload(), ctx.payload_size())) {
            ctx.set_state(BinaryContext::READY);
            uint8_t ok = 1;
            ctx.send(PROTO_HANDSHAKE_OK, &ok, 1);
        } else {
            // 握手失败
            ctx.raw_connection()->close();
        }
    }

    void handle_rpc_call(BinaryContext& ctx) {
        // 解析 RPC 请求（如 Protobuf）
        RpcRequest req = deserialize_rpc(ctx.payload(), ctx.payload_size());

        // 异步执行
        ctx.send_async([this, ctx, req]() {
            auto result = execute_rpc_method(req);
            auto data = serialize_rpc(result);
            ctx.send(PROTO_RPC_RESPONSE, data.data(), data.size());
        });
    }
};
```

---

## 5. Level 3: Data Process (传输层)

### 5.1 设计理念
- 完全控制字节流处理
- 保留旧框架的继承模式
- 适合极度定制的场景

### 5.2 核心接口

```cpp
// 继续使用现有的 base_data_process
class base_data_process {
public:
    // 协议检测（静态方法，供注册使用）
    static bool can_handle(const char* buf, size_t len) {
        return false;
    }

    // 工厂方法（静态方法，供注册使用）
    static std::unique_ptr<base_data_process> create(
        std::shared_ptr<base_net_obj> conn) {
        return nullptr;
    }

    // 数据处理（子类实现）
    virtual size_t process_recv_buf(const char* buf, size_t len) = 0;
    virtual std::string* get_send_buf() = 0;

    // 生命周期
    virtual void handle_msg(std::shared_ptr<normal_msg>& msg) {}
    virtual void handle_timeout(std::shared_ptr<timer_msg>& t_msg) {}
    virtual void peer_close() {}
    virtual void reset() {}
    virtual void destroy() {}

protected:
    std::shared_ptr<base_net_obj> get_base_net();
};
```

### 5.3 使用示例

```cpp
// 示例: 游戏协议（加密 + 状态机）
class GameProtocolProcess : public base_data_process {
public:
    GameProtocolProcess(std::shared_ptr<base_net_obj> conn)
        : base_data_process(conn), _state(State::INIT) {}

    // 协议检测
    static bool can_handle(const char* buf, size_t len) {
        return len >= 4 &&
               buf[0] == 'G' && buf[1] == 'A' &&
               buf[2] == 'M' && buf[3] == 'E';
    }

    // 工厂方法
    static std::unique_ptr<base_data_process> create(
        std::shared_ptr<base_net_obj> conn) {
        return std::make_unique<GameProtocolProcess>(conn);
    }

    // 数据处理
    size_t process_recv_buf(const char* buf, size_t len) override {
        _recv_buffer.append(buf, len);
        size_t consumed = 0;

        while (true) {
            switch (_state) {
                case State::INIT:
                    consumed = parse_handshake();
                    if (consumed == 0) return len;
                    break;

                case State::ENCRYPTED:
                    consumed = parse_encrypted_frame();
                    if (consumed == 0) return len;
                    break;

                case State::GAME_RUNNING:
                    consumed = parse_game_packet();
                    if (consumed == 0) return len;
                    break;
            }
        }
    }

    std::string* get_send_buf() override {
        if (_send_queue.empty()) return nullptr;

        auto& packet = _send_queue.front();
        _encrypted_send = _crypto.encrypt(packet);
        _send_queue.pop_front();

        return &_encrypted_send;
    }

private:
    enum class State { INIT, ENCRYPTED, GAME_RUNNING };

    size_t parse_handshake() {
        if (_recv_buffer.size() < 16) return 0;

        uint32_t version = read_u32(_recv_buffer, 4);
        uint32_t key = read_u32(_recv_buffer, 8);

        if (version != GAME_PROTOCOL_VERSION) {
            peer_close();
            return 0;
        }

        _crypto.init(key);
        _state = State::ENCRYPTED;

        _recv_buffer.erase(0, 16);
        return 16;
    }

    size_t parse_encrypted_frame() {
        if (_recv_buffer.size() < 8) return 0;

        uint32_t encrypted_len = read_u32(_recv_buffer, 4);
        if (_recv_buffer.size() < 8 + encrypted_len) return 0;

        auto decrypted = _crypto.decrypt(
            _recv_buffer.data() + 8, encrypted_len);

        process_game_packet(decrypted);

        _recv_buffer.erase(0, 8 + encrypted_len);
        return 8 + encrypted_len;
    }

    void process_game_packet(const std::vector<uint8_t>& packet) {
        uint8_t opcode = packet[0];

        switch (opcode) {
            case OP_PLAYER_MOVE:
                handle_player_move(packet.data() + 1, packet.size() - 1);
                break;
            case OP_ATTACK:
                handle_attack(packet.data() + 1, packet.size() - 1);
                break;
        }
    }

private:
    State _state;
    std::string _recv_buffer;
    std::deque<std::vector<uint8_t>> _send_queue;
    std::string _encrypted_send;
    CryptoEngine _crypto;
};
```

---

## 6. 统一协议工厂（UnifiedProtocolFactory）

### 6.1 设计理念
- 统一的协议注册和检测机制
- 支持三个层次的混合使用
- 运行时动态扩展

### 6.2 核心实现

```cpp
// core/unified_protocol_factory.h
namespace myframe {

class UnifiedProtocolFactory : public IFactory {
public:
    // 协议检测函数类型
    using DetectFn = std::function<bool(const char*, size_t)>;
    using CreateFn = std::function<std::unique_ptr<base_data_process>(std::shared_ptr<base_net_obj>)>;

    UnifiedProtocolFactory() = default;
    virtual ~UnifiedProtocolFactory() = default;

    // ========== Level 1: Application Handler ==========
    template<typename Handler>
    UnifiedProtocolFactory& register_http_handler(Handler* handler) {
        auto detect = [](const char* buf, size_t len) -> bool {
            if (len < 4) return false;
            return (memcmp(buf, "GET ", 4) == 0 ||
                    memcmp(buf, "POST ", 5) == 0 ||
                    memcmp(buf, "PUT ", 4) == 0 ||
                    memcmp(buf, "DELETE ", 7) == 0);
        };

        auto create = [handler](std::shared_ptr<base_net_obj> conn) {
            return std::make_unique<HttpApplicationAdapter>(conn, handler);
        };

        return register_protocol("http", detect, create, 10);
    }

    template<typename Handler>
    UnifiedProtocolFactory& register_ws_handler(Handler* handler) {
        auto detect = [](const char* buf, size_t len) -> bool {
            std::string data(buf, std::min(len, size_t(512)));
            return data.find("Upgrade: websocket") != std::string::npos ||
                   data.find("Upgrade: WebSocket") != std::string::npos;
        };

        auto create = [handler](std::shared_ptr<base_net_obj> conn) {
            return std::make_unique<WsApplicationAdapter>(conn, handler);
        };

        return register_protocol("websocket", detect, create, 20);
    }

    template<typename Handler>
    UnifiedProtocolFactory& register_binary_handler(
        const std::string& name,
        DetectFn detect,
        Handler* handler,
        int priority = 50)
    {
        auto create = [handler](std::shared_ptr<base_net_obj> conn) {
            return std::make_unique<BinaryApplicationAdapter>(conn, handler);
        };

        return register_protocol(name, detect, create, priority);
    }

    // ========== Level 2: Protocol Context Handler ==========
    template<typename Handler>
    UnifiedProtocolFactory& register_http_context_handler(Handler* handler) {
        auto detect = [](const char* buf, size_t len) -> bool {
            if (len < 4) return false;
            return (memcmp(buf, "GET ", 4) == 0 || memcmp(buf, "POST ", 5) == 0);
        };

        auto create = [handler](std::shared_ptr<base_net_obj> conn) {
            return std::make_unique<HttpContextAdapter>(conn, handler);
        };

        return register_protocol("http_ctx", detect, create, 10);
    }

    template<typename Handler>
    UnifiedProtocolFactory& register_ws_context_handler(Handler* handler) {
        auto detect = [](const char* buf, size_t len) -> bool {
            std::string data(buf, std::min(len, size_t(512)));
            return data.find("Upgrade: websocket") != std::string::npos;
        };

        auto create = [handler](std::shared_ptr<base_net_obj> conn) {
            return std::make_unique<WsContextAdapter>(conn, handler);
        };

        return register_protocol("ws_ctx", detect, create, 20);
    }

    template<typename Handler>
    UnifiedProtocolFactory& register_binary_context_handler(
        const std::string& name,
        DetectFn detect,
        Handler* handler,
        int priority = 50)
    {
        auto create = [handler](std::shared_ptr<base_net_obj> conn) {
            return std::make_unique<BinaryContextAdapter>(conn, handler);
        };

        return register_protocol(name, detect, create, priority);
    }

    // ========== Level 3: Custom Data Process ==========
    UnifiedProtocolFactory& register_custom_protocol(
        const std::string& name,
        DetectFn detect,
        CreateFn create,
        int priority = 100)
    {
        return register_protocol(name, detect, create, priority);
    }

    // 便捷方法（使用静态方法）
    template<typename ProcessClass>
    UnifiedProtocolFactory& register_custom_protocol_class(
        const std::string& name,
        int priority = 100)
    {
        auto create = [](std::shared_ptr<base_net_obj> conn) {
            return ProcessClass::create(conn);
        };

        return register_protocol(name, ProcessClass::can_handle, create, priority);
    }

    // IFactory 接口实现
    void net_thread_init(base_net_thread* th) override;
    void handle_thread_msg(std::shared_ptr<normal_msg>& msg) override;
    void register_worker(uint32_t thread_index) override;
    void on_accept(base_net_thread* listen_th, int fd) override;

private:
    struct ProtocolEntry {
        std::string name;
        DetectFn detect;
        CreateFn create;
        int priority;  // 数字越小优先级越高

        bool operator<(const ProtocolEntry& other) const {
            return priority < other.priority;
        }
    };

    UnifiedProtocolFactory& register_protocol(
        const std::string& name,
        DetectFn detect,
        CreateFn create,
        int priority);

    std::vector<ProtocolEntry> _protocols;
    std::vector<uint32_t> _worker_indices;
    uint32_t _rr_hint{0};
    common_obj_container* _container{nullptr};
};

} // namespace myframe
```

### 6.3 协议检测器实现

```cpp
// core/protocol_detector.h
namespace myframe {

// 协议检测和分发器
class ProtocolDetector : public base_data_process {
public:
    ProtocolDetector(
        std::shared_ptr<base_net_obj> conn,
        const std::vector<UnifiedProtocolFactory::ProtocolEntry>& protocols)
        : base_data_process(conn)
        , _protocols(protocols)
        , _detected(false)
    {
        // 按优先级排序
        std::sort(_protocols.begin(), _protocols.end());
    }

    size_t process_recv_buf(const char* buf, size_t len) override {
        if (!_detected) {
            _buffer.append(buf, len);

            // 遍历所有注册的协议
            for (auto& proto : _protocols) {
                if (proto.detect(_buffer.data(), _buffer.size())) {
                    // 协议匹配成功，创建处理器
                    _delegate = proto.create(get_base_net());
                    _detected = true;

                    PDEBUG("[protocol] %s selected", proto.name.c_str());

                    // 将缓冲的数据交给代理处理
                    size_t consumed = _delegate->process_recv_buf(
                        _buffer.data(), _buffer.size());
                    _buffer.clear();

                    return consumed > 0 ? consumed : len;
                }
            }

            // 检查缓冲区大小，防止恶意攻击
            if (_buffer.size() > MAX_DETECT_BUFFER_SIZE) {
                PDEBUG("[protocol] detection buffer overflow, closing");
                peer_close();
                return 0;
            }

            return len;  // 需要更多数据
        }

        // 已检测到协议，直接转发
        return _delegate ? _delegate->process_recv_buf(buf, len) : len;
    }

    std::string* get_send_buf() override {
        return _delegate ? _delegate->get_send_buf() : nullptr;
    }

    void handle_msg(std::shared_ptr<normal_msg>& msg) override {
        if (_delegate) _delegate->handle_msg(msg);
    }

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (_delegate) _delegate->handle_timeout(t_msg);
    }

    void peer_close() override {
        if (_delegate) _delegate->peer_close();
        base_data_process::peer_close();
    }

    void reset() override {
        _detected = false;
        _buffer.clear();
        _delegate.reset();
        base_data_process::reset();
    }

    void destroy() override {
        if (_delegate) _delegate->destroy();
        base_data_process::destroy();
    }

private:
    std::vector<UnifiedProtocolFactory::ProtocolEntry> _protocols;
    std::unique_ptr<base_data_process> _delegate;
    bool _detected;
    std::string _buffer;

    static constexpr size_t MAX_DETECT_BUFFER_SIZE = 4096;  // 4KB
};

} // namespace myframe
```

---

## 7. 适配器实现

### 7.1 Level 1 适配器

```cpp
// core/protocol_adapters.h
namespace myframe {

// HTTP Level 1 适配器
class HttpApplicationAdapter : public http_res_process {
public:
    HttpApplicationAdapter(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler)
        : http_res_process(conn)
        , _handler(handler)
    {
        set_process(new HttpApplicationDataProcess(this, handler));
    }

private:
    IApplicationHandler* _handler;
};

class HttpApplicationDataProcess : public http_base_data_process {
public:
    HttpApplicationDataProcess(
        http_base_process* process,
        IApplicationHandler* handler)
        : http_base_data_process(process)
        , _handler(handler)
    {}

    void msg_recv_finish() override {
        // 构造 HttpRequest
        HttpRequest req;
        auto& head = _base_process->get_req_head_para();
        req.method = head._method;
        req.url = head._url;
        req.version = head._version;
        req.headers = head._head_map;
        req.body = get_recv_body();

        // 构造 HttpResponse
        HttpResponse res;

        // 调用用户处理器
        _handler->on_http(req, res);

        // 设置响应
        auto& res_head = _base_process->get_res_head_para();
        res_head._status_code = res.status;
        res_head._reason = res.reason;
        res_head._head_map = res.headers;
        set_send_body(res.body);
    }

private:
    IApplicationHandler* _handler;
};

// WebSocket Level 1 适配器
class WsApplicationAdapter : public web_socket_res_process {
public:
    WsApplicationAdapter(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler)
        : web_socket_res_process(conn)
        , _handler(handler)
    {
        set_process(new WsApplicationDataProcess(this, handler));
    }

private:
    IApplicationHandler* _handler;
};

class WsApplicationDataProcess : public web_socket_data_process {
public:
    WsApplicationDataProcess(
        web_socket_process* process,
        IApplicationHandler* handler)
        : web_socket_data_process(process)
        , _handler(handler)
    {}

    void msg_recv_finish() override {
        // 构造 WsFrame
        WsFrame recv;
        recv.opcode = static_cast<WsFrame::OpCode>(get_opcode());
        recv.payload = get_payload();
        recv.fin = is_fin();

        WsFrame send;

        // 调用用户处理器
        _handler->on_ws(recv, send);

        // 发送响应
        send_frame(send.opcode, send.payload.data(), send.payload.size(), send.fin);
    }

private:
    IApplicationHandler* _handler;
};

// Binary Level 1 适配器
class BinaryApplicationAdapter : public base_data_process {
public:
    BinaryApplicationAdapter(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler)
        : base_data_process(conn)
        , _handler(handler)
    {}

    size_t process_recv_buf(const char* buf, size_t len) override {
        _recv_buffer.append(buf, len);

        // 假设简单的 TLV 格式: [4字节协议ID][4字节长度][数据]
        while (_recv_buffer.size() >= 8) {
            uint32_t proto_id = read_u32(_recv_buffer, 0);
            uint32_t data_len = read_u32(_recv_buffer, 4);

            if (_recv_buffer.size() < 8 + data_len) break;

            // 构造请求
            BinaryRequest req;
            req.protocol_id = proto_id;
            req.payload.assign(
                _recv_buffer.begin() + 8,
                _recv_buffer.begin() + 8 + data_len);

            BinaryResponse res;

            // 调用用户处理器
            _handler->on_binary(req, res);

            // 发送响应
            send_binary_response(proto_id, res.data);

            _recv_buffer.erase(0, 8 + data_len);
        }

        return len;
    }

    std::string* get_send_buf() override {
        if (_send_queue.empty()) return nullptr;
        _current_send = std::move(_send_queue.front());
        _send_queue.pop_front();
        return &_current_send;
    }

private:
    void send_binary_response(uint32_t proto_id, const std::vector<uint8_t>& data) {
        std::string frame;
        frame.reserve(8 + data.size());

        write_u32(frame, proto_id);
        write_u32(frame, data.size());
        frame.append(reinterpret_cast<const char*>(data.data()), data.size());

        _send_queue.push_back(std::move(frame));
    }

    IApplicationHandler* _handler;
    std::string _recv_buffer;
    std::deque<std::string> _send_queue;
    std::string _current_send;
};

} // namespace myframe
```

### 7.2 Level 2 适配器

```cpp
// Level 2 适配器实现类似，但使用 IProtocolHandler 接口
// 并提供 Context 对象而不是简单的 Request/Response

class HttpContextAdapter : public http_res_process {
    // 提供 HttpContext 给用户处理器
};

class WsContextAdapter : public web_socket_res_process {
    // 提供 WsContext 给用户处理器
};

class BinaryContextAdapter : public base_data_process {
    // 提供 BinaryContext 给用户处理器
};
```

---

## 8. 使用示例

### 8.1 简单场景（Level 1）

```cpp
// 示例：HTTP + WebSocket 服务器
#include "myframe/server.h"
#include "myframe/unified_protocol_factory.h"

class MyApp : public IApplicationHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        if (req.url == "/api/hello") {
            res.set_json("{\"message\":\"Hello\"}");
        } else {
            res.status = 404;
        }
    }

    void on_ws(const WsFrame& recv, WsFrame& send) override {
        send = WsFrame::text("echo: " + recv.payload);
    }
};

int main() {
    server srv(2);  // 1 listen + 1 worker

    auto handler = std::make_shared<MyApp>();
    auto factory = std::make_shared<UnifiedProtocolFactory>();

    factory->register_http_handler(handler.get())
           .register_ws_handler(handler.get());

    srv.bind("0.0.0.0", 8080);
    srv.set_business_factory(factory);
    srv.start();
    srv.join();

    return 0;
}
```

### 8.2 复杂场景（Level 2）

```cpp
// 示例：带认证的 WebSocket 服务器
class AuthWsHandler : public IProtocolHandler {
public:
    void on_ws_frame(WsContext& ctx) override {
        bool* auth = static_cast<bool*>(ctx.get_user_data("auth"));

        if (!auth || !*auth) {
            // 未认证
            auto& frame = ctx.frame();
            if (frame.payload.find("token:") == 0) {
                if (validate_token(frame.payload.substr(6))) {
                    bool* authenticated = new bool(true);
                    ctx.set_user_data("auth", authenticated);
                    ctx.send_text("auth_ok");
                } else {
                    ctx.close(1008, "Invalid token");
                }
            } else {
                ctx.send_text("auth_required");
            }
        } else {
            // 已认证，处理业务
            handle_message(ctx);
        }
    }
};

int main() {
    server srv(2);
    auto handler = std::make_shared<AuthWsHandler>();
    auto factory = std::make_shared<UnifiedProtocolFactory>();

    factory->register_ws_context_handler(handler.get());

    srv.bind("0.0.0.0", 8080);
    srv.set_business_factory(factory);
    srv.start();
    srv.join();

    return 0;
}
```

### 8.3 混合场景（多协议 + 多层次）

```cpp
// 示例：HTTP(L1) + WebSocket(L2) + 自定义协议(L3)
int main() {
    server srv(4);  // 1 listen + 3 workers

    // Level 1: 简单 HTTP
    auto http_handler = std::make_shared<SimpleHttpHandler>();

    // Level 2: 复杂 WebSocket
    auto ws_handler = std::make_shared<AuthWsHandler>();

    // Level 3: 自定义游戏协议
    // GameProtocolProcess 已实现 can_handle() 和 create()

    auto factory = std::make_shared<UnifiedProtocolFactory>();
    factory->register_http_handler(http_handler.get())               // L1
           .register_ws_context_handler(ws_handler.get())             // L2
           .register_custom_protocol_class<GameProtocolProcess>(     // L3
               "game", 50);

    srv.bind("0.0.0.0", 8080);
    srv.set_business_factory(factory);
    srv.start();
    srv.join();

    return 0;
}
```

---

## 9. 实施计划

### 9.1 Phase 1: 核心框架（2周）
- [ ] 实现 `UnifiedProtocolFactory`
- [ ] 实现 `ProtocolDetector`
- [ ] 实现 Level 1 接口和适配器
- [ ] 单元测试

### 9.2 Phase 2: Level 2 支持（2周）
- [ ] 实现 Context 类（HttpContext, WsContext, BinaryContext）
- [ ] 实现 Level 2 适配器
- [ ] 异步响应支持
- [ ] 集成测试

### 9.3 Phase 3: 文档和示例（1周）
- [ ] API 文档
- [ ] 使用示例
- [ ] 迁移指南
- [ ] 性能测试

### 9.4 Phase 4: 向后兼容和迁移（1周）
- [ ] 提供从旧 `MultiProtocolFactory` 的迁移工具
- [ ] 兼容性测试
- [ ] 发布 Beta 版本

---

## 10. 设计优势总结

| 特性 | 旧设计 | 新设计 |
|------|--------|--------|
| 简单场景易用性 | 中等 | 优秀（Level 1） |
| 复杂场景控制力 | 有限 | 强大（Level 2/3） |
| 协议扩展性 | 需改框架 | 运行时注册 |
| 向后兼容性 | 不兼容 | 完全兼容（Level 3） |
| 协议检测统一性 | 分散 | 统一机制 |
| 学习曲线 | 陡峭 | 渐进式 |
| 二进制协议支持 | 困难 | 原生支持 |

---

## 11. 附录

### 11.1 协议优先级建议

| 优先级 | 协议类型 | 说明 |
|--------|---------|------|
| 0-9 | 特殊协议 | TLS握手等 |
| 10-19 | HTTP | 常见文本协议 |
| 20-29 | WebSocket | 升级协议 |
| 30-49 | 其他文本协议 | Redis、SMTP等 |
| 50-79 | 二进制协议 | RPC、自定义等 |
| 80-99 | 兜底协议 | 默认处理 |
| 100+ | 自定义 | 用户自由分配 |

### 11.2 Context 数据管理最佳实践

```cpp
// 使用 RAII 管理 Context 用户数据
class ContextDataGuard {
public:
    ContextDataGuard(HttpContext& ctx, const std::string& key, void* data)
        : ctx_(ctx), key_(key) {
        ctx_.set_user_data(key_, data);
    }

    ~ContextDataGuard() {
        void* data = ctx_.get_user_data(key_);
        if (data) {
            delete static_cast<YourType*>(data);
        }
    }

private:
    HttpContext& ctx_;
    std::string key_;
};
```

---

**文档版本**: 1.0
**最后更新**: 2025-10-22
**作者**: MyFrame Team
**状态**: 设计评审中
