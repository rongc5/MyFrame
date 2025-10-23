# MyFrame 示例程序索引

**最后更新**: 2025-10-22

本目录包含 MyFrame 框架的所有示例程序，按功能分类如下：

---

## ? 分类索引

### ? 统一协议架构示例 (Unified Protocol Architecture)

**推荐新用户从这些示例开始**

| 示例 | 功能 | Level | 说明 |
|------|------|-------|------|
| `unified_simple_http.cpp` | HTTP 服务器 | Level 1 | 最简单的 HTTP 示例 ? |
| `unified_mixed_server.cpp` | HTTP + WebSocket | Level 1 | 混合协议服务器 |
| `unified_level2_demo.cpp` | HTTP Context | Level 2 | Level 2 高级特性 |
| `unified_https_wss_server.cpp` | HTTPS + WSS | Level 1 | TLS 加密示例 |
| `unified_async_demo.cpp` | 异步响应 | Level 2 | 异步和消息处理 |
| `unified_ws_client_test.cpp` | WebSocket 客户端 | - | WS 客户端测试 |

### ?? 异步响应和定时器

| 示例 | 功能 | 说明 |
|------|------|------|
| `simple_async_test.cpp` | 异步 + 定时器 | **推荐入门** - 定时器使用示例 ? |
| `async_http_server_demo.cpp` | 完整异步服务器 | 完整的异步响应演示 |

### ? 基础 HTTP/HTTPS 示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `simple_http_server.cpp` | HTTP 服务器 | 基础 HTTP 服务器 |
| `simple_https_server.cpp` | HTTPS 服务器 | TLS 加密的 HTTP |
| `simple_wss_server.cpp` | WSS 服务器 | TLS 加密的 WebSocket |

### ? HTTP/2 示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `simple_h2_server.cpp` | HTTP/2 服务器 | 支持 ALPN 和 h2 |
| `simple_h2_client.cpp` | HTTP/2 客户端 | 简单的 h2 客户端 |
| `http2_out_client.cpp` | HTTP/2 出站客户端 | 使用 out_connect |

### ? WebSocket 示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `ws_broadcast_user.cpp` | 用户广播 | WebSocket 广播消息 |
| `ws_broadcast_periodic.cpp` | 周期性广播 | 定时广播示例 |
| `ws_bench_client.cpp` | 性能测试客户端 | WS 压力测试 |
| `ws_stickbridge_client.cpp` | StickBridge 客户端 | 特殊 WS 客户端 |

### ? 客户端示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `http_out_client.cpp` | HTTP 出站客户端 | 基础 HTTP 客户端 |
| `biz_http_client.cpp` | 业务 HTTP 客户端 | 业务层客户端 |
| `router_client.cpp` | 路由客户端 | 统一路由客户端 |
| `router_biz_client.cpp` | 业务路由客户端 | 带业务逻辑的路由 |
| `client_conn_factory_example.cpp` | 客户端连接工厂 | 连接工厂示例 |

### ? 高级功能示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `multi_protocol_server.cpp` | 多协议服务器 | 支持多种协议 |
| `multi_thread_server.cpp` | 多线程服务器 | 多线程处理 |
| `http_server_close_demo.cpp` | 优雅关闭 | HTTP 服务器关闭 |
| `http_close_demo.cpp` | 连接关闭 | 连接关闭演示 |

### ? 自定义协议示例

| 示例 | 功能 | 说明 |
|------|------|------|
| `xproto_server.cpp` | 自定义协议服务器 | 使用 MultiProtocolFactory |
| `xproto_client.cpp` | 自定义协议客户端 | 自定义协议客户端 |

### ? 股票系统示例 (可选)

| 示例 | 功能 | 说明 |
|------|------|------|
| `stock_index_server.cpp` | 行情服务器 | 股票行情推送 |
| `stock_bridge_server.cpp` | 桥接服务器 | 数据桥接 |
| `stock_driver.cpp` | 驱动程序 | 数据驱动 |

**注**: 股票系统示例需要相应的源文件才会编译。

---

## ? 新手入门路线

### 第一步：最简单的 HTTP 服务器

```bash
# 编译
cd build
make unified_simple_http

# 运行
./examples/unified_simple_http 8080

# 测试
curl http://127.0.0.1:8080/
```

**文件**: `unified_simple_http.cpp`  
**特点**: 仅 100 行代码，最容易理解

---

### 第二步：异步响应和定时器

```bash
# 编译
make simple_async_test

# 运行
./examples/simple_async_test 8080

# 测试
curl http://127.0.0.1:8080/async
```

**文件**: `simple_async_test.cpp`  
**特点**: 学习异步处理和定时器使用

---

### 第三步：Level 2 高级特性

```bash
# 编译
make unified_level2_demo

# 运行
./examples/unified_level2_demo 8080

# 测试
curl http://127.0.0.1:8080/async
```

**文件**: `unified_level2_demo.cpp`  
**特点**: Context API、流式响应、异步控制

---

### 第四步：HTTPS/WSS 加密通信

```bash
# 编译
make unified_https_wss_server

# 运行
./examples/unified_https_wss_server 8443

# 测试
curl -k https://127.0.0.1:8443/
```

**文件**: `unified_https_wss_server.cpp`  
**特点**: TLS 加密、HTTPS、WSS

---

## ? 编译所有示例

```bash
cd /home/rong/myframe
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

所有示例将编译到 `build/examples/` 目录。

---

## ? 学习路径建议

### 初级（1-2天）

1. ? `unified_simple_http.cpp` - HTTP 基础
2. ? `simple_async_test.cpp` - 异步和定时器
3. ? `unified_mixed_server.cpp` - HTTP + WebSocket

### 中级（3-5天）

4. ? `unified_level2_demo.cpp` - Level 2 Context API
5. ? `unified_https_wss_server.cpp` - TLS 加密
6. ? `async_http_server_demo.cpp` - 完整异步服务器
7. ? `simple_h2_server.cpp` - HTTP/2

### 高级（1-2周）

8. ? `multi_protocol_server.cpp` - 多协议
9. ? `multi_thread_server.cpp` - 多线程
10. ? `xproto_server.cpp` - 自定义协议
11. ? 客户端示例 - 各种客户端实现

---

## ? 配套文档

### 统一协议框架

- `docs/UNIFIED_PROTOCOL_DESIGN.md` - 设计文档
- `docs/UNIFIED_PROTOCOL_COMPLETE.md` - 完整指南
- `docs/UNIFIED_PROTOCOL_STATUS.md` - 实现状态

### 异步响应和定时器

- `docs/ASYNC_TIMER_QUICK_REF.md` - **快速参考** ?
- `docs/TIMER_USAGE_GUIDE.md` - 定时器详细指南
- `docs/ASYNC_RESPONSE_GUIDE.md` - 异步响应指南
- `docs/ASYNC_AND_TIMER_COMPLETE.md` - 完整实现总结

### 其他功能

- `docs/THREAD_ACCESS_GUIDE.md` - 线程访问指南
- `examples/README_ASYNC_HTTP.md` - 异步 HTTP 说明
- `examples/README_NEW_EXAMPLES.md` - 新示例说明
- `examples/README_STOCK_SYSTEM.md` - 股票系统说明

---

## ? 按功能查找示例

### 我想学习...

#### HTTP 服务器
→ `unified_simple_http.cpp` (最简单)  
→ `simple_http_server.cpp` (传统方式)

#### 异步处理
→ `simple_async_test.cpp` (推荐)  
→ `async_http_server_demo.cpp` (完整)

#### WebSocket
→ `unified_mixed_server.cpp` (HTTP + WS)  
→ `ws_broadcast_user.cpp` (广播)

#### HTTPS/TLS
→ `unified_https_wss_server.cpp` (推荐)  
→ `simple_https_server.cpp` (传统)

#### HTTP/2
→ `simple_h2_server.cpp` (服务器)  
→ `http2_out_client.cpp` (客户端)

#### 自定义协议
→ `xproto_server.cpp` + `xproto_client.cpp`

#### 客户端开发
→ `router_client.cpp` (统一路由)  
→ `biz_http_client.cpp` (业务客户端)

---

## ?? 故障排除

### 编译错误

```bash
# 1. 确保依赖已安装
sudo apt-get install libssl-dev

# 2. 清理并重新编译
cd build
rm -rf *
cmake ..
make -j$(nproc)
```

### 运行错误

```bash
# 1. 检查端口是否被占用
netstat -tulpn | grep 8080

# 2. 使用其他端口
./examples/unified_simple_http 9090

# 3. 查看详细日志
export MYFRAME_DEBUG=1
./examples/unified_simple_http 8080
```

### SSL 证书问题

```bash
# 生成测试证书
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes

# 使用 curl 测试 HTTPS（忽略证书验证）
curl -k https://127.0.0.1:8443/
```

---

## ? 示例命名规范

- `simple_*.cpp` - 基础示例，适合入门
- `unified_*.cpp` - 使用统一协议框架的示例
- `*_demo.cpp` - 功能演示程序
- `*_client.cpp` - 客户端程序
- `*_server.cpp` - 服务器程序

---

## ? 贡献指南

添加新示例时：

1. 在 `CMakeLists.txt` 中添加编译目标
2. 更新本 README 文档
3. 添加详细的代码注释
4. 提供运行和测试说明

---

## ? 获取帮助

- 查看 `docs/` 目录下的详细文档
- 阅读示例代码中的注释
- 参考 `README*.md` 文件

---

**提示**: 从 `unified_simple_http.cpp` 开始，逐步学习更高级的功能！

**最后更新**: 2025-10-22

