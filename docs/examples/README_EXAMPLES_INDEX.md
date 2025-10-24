# MyFrame 示例索引

**更新日期**: 2025-10-22

本目录列出仓库中可直接运行的示例程序，按功能分类。

---

## 统一协议架构 (Unified Protocol Architecture)

| 示例 | 功能 | Level | 说明 |
|------|------|-------|------|
| `unified_simple_http.cpp` | HTTP 服务器 | Level 1 | 最简 HTTP 示例 |
| `unified_mixed_server.cpp` | HTTP + WebSocket | Level 1 | 多协议同端口 |
| `unified_level2_demo.cpp` | HTTP Context | Level 2 | Level 2 进阶功能 |
| `unified_https_wss_server.cpp` | HTTPS + WSS | Level 1 | TLS 演示 |
| `unified_async_demo.cpp` | 异步响应 | Level 2 | Async/消息示例 |
| `level1_multi_protocol.cpp` | HTTP/WS/HTTPS/WSS/HTTP2 | Level 1 | 新增 Level1 多协议演示 |
| `level2_multi_protocol.cpp` | HTTP/WS/Binary | Level 2 | 新增 Level2 多协议 + 异步 |
| `level3_custom_echo.cpp` | RAW 自定义协议 | Level 3 | 直接继承 base_data_process |
| `unified_ws_client_test.cpp` | WebSocket 客户端 | - | WebSocket 测试 |

## 异步与定时器

| 示例 | 功能 | 说明 |
|------|------|------|
| `simple_async_test.cpp` | 定时器 + 异步 | 基础定时器示例 |
| `async_http_server_demo.cpp` | 异步 HTTP | 展示跨线程响应 |

## HTTP/HTTPS 示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `simple_http_server.cpp` | HTTP 服务器 | 经典 HTTP 示例 |
| `simple_https_server.cpp` | HTTPS 服务器 | TLS 加密 |
| `simple_wss_server.cpp` | WSS 服务器 | TLS 上的 WebSocket |

## HTTP/2 示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `simple_h2_server.cpp` | HTTP/2 服务器 | 含 ALPN h2 |
| `simple_h2_client.cpp` | HTTP/2 客户端 | h2 客户端示例 |
| `http2_out_client.cpp` | HTTP/2 出站客户端 | out_connect 示例 |

## WebSocket 示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `ws_broadcast_user.cpp` | 用户广播 | WebSocket 广播示例 |
| `ws_broadcast_periodic.cpp` | 定时广播 | 定期推送 |
| `ws_bench_client.cpp` | 压测客户端 | WebSocket 压测 |
| `ws_stickbridge_client.cpp` | StickBridge 客户端 | 业务场景 |

## 客户端示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `http_out_client.cpp` | HTTP 出站客户端 | 基本请求 |
| `biz_http_client.cpp` | 业务 HTTP 客户端 | 业务逻辑示例 |
| `router_client.cpp` | 统一路由客户端 | 多协议路由 |
| `router_biz_client.cpp` | 业务路由客户端 | 含业务逻辑 |
| `client_conn_factory_example.cpp` | 客户端连接工厂 | 动态构建连接 |

## 高级功能

| 示例 | 功能 | 说明 |
|------|------|------|
| `multi_protocol_server.cpp` | 多协议服务器 | 支持 HTTP/HTTPS/WS |
| `multi_thread_server.cpp` | 多线程服务器 | 工作线程调度 |
| `http_server_close_demo.cpp` | HTTP 关闭流程 | 响应后关闭连接 |
| `http_close_demo.cpp` | 连接关闭 | 延迟关闭示例 |

## 自定义协议

| 示例 | 功能 | 说明 |
|------|------|------|
| `xproto_server.cpp` | 自定义协议服务器 | 基于 MultiProtocolFactory |
| `xproto_client.cpp` | 自定义协议客户端 | 配合服务器 |

## 股票系统示例（可选）

| 示例 | 功能 | 说明 |
|------|------|------|
| `stock_index_server.cpp` | 指数服务 | 股票示例 |
| `stock_bridge_server.cpp` | 桥接服务 | 股票示例 |
| `stock_driver.cpp` | 驱动 | 股票示例 |

---

更多使用说明请参考 `docs/examples/README_PROTOCOL_LEVELS.md`。
