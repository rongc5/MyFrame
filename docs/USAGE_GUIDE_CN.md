## MyFrame 使用指南（客户端 / 服务端 / 多协议 / 自定义协议）

本指南介绍如何在 MyFrame 中：
- 使用服务端框架并注册多种协议（HTTP/HTTPS、WS/WSS、自定义协议）
- 使用客户端（事件循环版）访问多种协议
- 扩展并集成自定义协议（服务端与客户端）

### 1. 服务端：多协议监听与分发

- 入口类：`server`（参见 `include/server.h`）
- 业务工厂：`MultiProtocolFactory`（参见 `include/multi_protocol_factory.h`）
  - 自动协议检测由 `protocol_detect_process` 完成，探测器在 `core/protocol_probes.h` 中定义：
    - `TlsProbe`、`HttpProbe`、`WsProbe`、`Http2Probe`、`CustomProbe`
- 最简示例：`examples/simple_https_server.cpp` / `examples/simple_wss_server.cpp` / `examples/xproto_server.cpp`

示例（摘自 `examples/xproto_server.cpp`）：
```cpp
auto handler = std::make_shared<XProtoHandler>(); // 业务回调，见 core/app_handler.h
auto biz = std::make_shared<MultiProtocolFactory>(handler.get(), MultiProtocolFactory::Mode::Auto);
server srv(2);
srv.bind("127.0.0.1", 7790);
srv.set_business_factory(biz);
srv.start();
srv.join();
```

要注册/控制探测器顺序：在 `core/multi_protocol_factory_impl.cpp` 内通过 `_mode` 选择/增删 `add_probe()` 即可。

### 2. 客户端：事件循环路由（HTTP/HTTPS/WS/WSS）

- 入口：`core/client_conn_router.h/.cpp` 提供 `ClientConnRouter`
- URL scheme 自动选择 builder：`http/https/ws/wss`
- HTTPS/WSS 使用 `ClientSslCodec`（`core/client_ssl_codec.h`）非阻塞 `SSL_connect`

示例：`examples/client_conn_factory_example.cpp`
```bash
build/examples/client_conn_factory_example http://127.0.0.1:7782/hello
build/examples/client_conn_factory_example https://127.0.0.1:443/
build/examples/client_conn_factory_example ws://127.0.0.1:7782/websocket
build/examples/client_conn_factory_example wss://127.0.0.1:443/ws
```

### 3. 直接使用 out_connect<PROCESS> 精细控制

适合需要过程级控制、复用框架事件循环的场景。
- HTTP 客户端：`out_connect<http_req_process>` + `http_client_data_process`
  - 示例：`examples/http_out_client.cpp`
- WS 客户端：`out_connect<web_socket_req_process>` + `web_socket_data_process`（或 `app_ws_data_process`）

### 4. 自定义协议（服务端）

步骤：
1) 在 `core/protocol_probes.h` 里新增 `class XProtoProbe : public IProtocolProbe`：
   - `match()`：基于魔数/前缀判断（例如前 4 字节为 `"XPRT"`）
   - `create()`：返回你的 `base_data_process` 实例（如 `xproto_process`）
2) 在 `protocol_detect_process` 初始化时 `add_probe(std::unique_ptr<XProtoProbe>(...))`
3) 在 `xproto_process::process_recv_buf()` 中解析帧并调用 `IAppHandler::on_custom(...)` 回调

快速起步：可复用 `custom_stream_process`（已在 `CustomProbe` 中示例化），它会把收到的数据回调给 `on_custom()` 并回显。

参考示例：`examples/xproto_server.cpp`

### 5. 自定义协议（客户端）

方案 A：基于路由器新增 scheme 对应的 builder
- 在 `ClientConnRouter` 中注册：
```cpp
router.register_factory("xproto", [](const std::string& url, const ClientBuildCtx& ctx){
    // 解析 URL -> host/port
    // auto conn = std::make_shared< out_connect<xproto_req_process> >(host, port);
    // auto proc = new xproto_req_process(conn);
    // proc->set_process(new xproto_client_data_process(proc, ...));
    // conn->set_process(proc); conn->set_net_container(ctx.container); conn->connect();
    // return std::static_pointer_cast<base_net_obj>(conn);
});
```

方案 B：基于现有 WS/HTTP 通道封装你的载荷（适合隧道化/快速验证）
- 直接用 `ws://`/`wss://` builder，数据交由服务端 `on_ws` 处理。

参考示例：`examples/xproto_client.cpp`（演示用 ws 通道）。

### 6. 运行示例

构建全部：
```bash
./scripts/build_cmake.sh all
```

服务端：
```bash
build/examples/xproto_server 7790
```

客户端（事件循环路由）：
```bash
build/examples/client_conn_factory_example ws://127.0.0.1:7790/
```

### 7. TLS 客户端配置

`ClientSslCodec` 默认不强制证书校验，可在后续版本扩展：
- 增加客户端 TLS 配置接口，设置 CA、校验开关与 SNI；
- 在安装 `ClientSslCodec` 前设置校验策略。

### 8. FAQ

- 如何改探测优先级？
  - 调整 `protocol_detect_process` 中 `add_probe()` 的顺序。
- 能支持 HTTP/2 客户端吗？
  - 当前是 HTTP/1.1 客户端。可新增 `h2_client`（含 ALPN、HPACK、帧流控），并注册到路由器。


