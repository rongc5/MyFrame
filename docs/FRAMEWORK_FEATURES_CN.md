# MyFrame 框架特性与近期变更说明

本文档汇总当前框架已支持的能力，以及本次交付中完成的关键优化与改动，便于快速了解使用方式与行为差异。

## 框架能力概览
- 协议与功能
  - HTTP/1.1 服务器与客户端（GET/POST、Content-Length/Chunked）。
  - WebSocket 服务器与客户端（WS/WSS，带业务回调钩子）。
  - HTTPS/WSS（TLS 1.2/1.3，支持 SNI；客户端证书校验可按需扩展）。
  - HTTP/2：
    - 服务器侧：TLS + ALPN `h2`（示例 `examples/simple_h2_server.cpp`）。
    - 客户端侧：事件循环客户端 `h2://` 路由；同步示例 `examples/simple_h2_client.cpp`。
- 事件循环与线程
  - `base_net_thread` + `common_obj_container` + `epoll` 事件驱动模型。
  - 支持将客户端连接对象加入容器，统一由事件循环驱动收发与超时处理。
  - 支持监听线程与 worker 线程池的分发（轮询）。
- 统一路由
  - `ClientConnRouter` 根据 URL scheme 选择构建器：`http/https/ws/wss/h2`。
  - 简化客户端接入事件循环的使用成本。

## 本次新增与优化
1) HTTPS 自动 ALPN 升级 HTTP/2（失败回退 HTTP/1.1）
- 新增 `core/hybrid_https_client_process.{h,cpp}`：
  - TLS 握手完成后读取 ALPN 结果；
  - 若协商到 `h2`，则委托到 `http2_client_process` 进行 HTTP/2 收发；
  - 否则按最小 HTTP/1.1 一次性客户端发送与解析（支持 Content-Length 与 chunked），完成后自动退出。
- `client_conn_router2.cpp`：`https` builder 切换为 `tls_out_connect<hybrid_https_client_process>`，ALPN 列表 `"h2,http/1.1"`。
- `client_ssl_codec.h`：新增 `selected_alpn()` 暴露握手后的 ALPN 协商结果。

2) HTTP/2 客户端增强
- `http2_client_process`：
  - 新增 PING 定时器（默认 15s）；收到对端 PING 自动 ACK。
  - 总超时（默认 30s）未完成则停止事件线程，避免长时间挂起。
 - 流控优化：将 WINDOW_UPDATE 改为按 32KB 阈值批量发送，减少控制帧开销。

4) 可调性能参数（环境变量/env）
- 监听 backlog：`MYFRAME_SOMAXCONN`（默认 1024）。
- TLS 会话：服务端 `MYFRAME_SSL_SESS_CACHE`(1/0) 与 `MYFRAME_SSL_SESS_CACHE_SIZE`，`MYFRAME_SSL_TICKETS`(1/0)。
- HTTP/2：`MYFRAME_H2_WINUPDATE`（字节，默认 32768）、`MYFRAME_H2_PING_MS`（默认 15000）、`MYFRAME_H2_TIMEOUT_MS`（默认 30000）。

3) 连接与事件稳定性
- `out_connect::connect()`：
  - 使用 `getaddrinfo` 支持域名解析与 IPv6；
  - 非阻塞连接并在失败时尝试后续地址条目。
- 默认监听 `EPOLLRDHUP`，并在 `event_process()` 中将 RDHUP 视为错误路径以便及时回收半关闭连接。
- 修正部分 `PDEBUG` 打印的类型与格式化（size_t/ssize_t）。

## 使用方式
- 构建
  - `./scripts/build_cmake.sh all`
- 统一事件循环示例
  - HTTP/1.1：`build/examples/router_client http://127.0.0.1:7782/hello`
  - HTTPS（优先 HTTP/2，失败回退 1.1）：`build/examples/router_client https://127.0.0.1:7779/hello`
  - 强制 HTTP/2：`build/examples/router_client h2://127.0.0.1:7779/hello`
- 业务化客户端（带头/方法/Body/WS 回显）：
  - `build/examples/router_biz_client http://127.0.0.1:7782/echo --method POST --H 'Content-Type: application/json' --data '{"x":1}'`

## 事件循环接入要点
- 通过 `ClientConnRouter::create_and_enqueue(url, ctx)`：
  - 在 builder 内部执行 `conn->set_net_container(ctx.container); conn->connect();` 并加入容器。
  - 所有网络事件由 `common_obj_container::obj_process()` 驱动（`real_net_process()` + `epoll_wait()`）。
- 手动路径（HTTP/1.1）：
  - 构建 `out_connect<http_req_process>`，设置 process -> `set_net_container(container)` -> `connect()`。

## 注意事项与后续方向
- 仍可进一步优化：
  - 切换 EPOLL 边沿触发（ET）与批量 I/O/零拷贝（需要配套缓冲与循环改造）。
  - 更完整的 HPACK/Huffman 支持与 header 映射（客户端/服务端）。
  - https 路径可选复用 `http_req_process/http_client_data_process` 作为回退实现（目前为最小内建实现，轻量可靠）。

## 默认请求头与压缩
- HTTP/1.1 客户端（未显式提供时自动注入）：
  - `User-Agent: myframe-http-client`
  - `Accept-Encoding: gzip`
  - 同时自动补全 `Host` 与 `Connection: close`
- HTTP/2 客户端（未显式提供时自动注入）：
  - `user-agent: myframe-h2-client`
  - `accept-encoding: gzip`
- 自动解压：
  - 若构建检测到 zlib，则对 gzip 响应尝试自动解压（HTTP/1.1 与 HTTP/2 一致），输出解压后长度与内容（小体积时）。
  - 未启用 zlib 时保持原样输出，不影响功能。
- 可选关闭默认 Accept-Encoding：
  - 通过环境变量 `MYFRAME_NO_DEFAULT_AE=1` 关闭 HTTP/1.1 与 HTTP/2 的默认 `Accept-Encoding: gzip` 注入。
  - `router_client` 支持命令行开关：`--no-accept-encoding`（内部设置上述环境变量）。
