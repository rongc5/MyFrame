# MyFrame 框架使用示例

这个目录包含了MyFrame框架的各种使用示例，展示如何使用框架的不同功能。

## 服务器示例

### 1. HTTP服务器 (`simple_http_server.cpp`)
基础HTTP服务器示例，展示如何：
- 创建HTTP服务器
- 处理HTTP请求和响应
- 设置路由和内容类型

**运行方式:**
```bash
./http_server [端口]
```

**测试命令:**
```bash
curl http://127.0.0.1:8080/hello
curl http://127.0.0.1:8080/api/status
```

### 2. HTTPS服务器 (`simple_https_server.cpp`)
HTTPS安全服务器示例，展示如何：
- 配置SSL/TLS加密
- 处理HTTPS请求
- 使用TLS模式

**运行方式:**
```bash
./https_server [端口]
```

**测试命令:**
```bash
curl -k https://127.0.0.1:7777/hello
curl -k https://127.0.0.1:7777/api/status
```

### 3. HTTP/2 服务器 (`simple_h2_server.cpp`)
基于TLS+ALPN的HTTP/2服务器，展示如何：
- 通过ALPN协商h2并处理HTTP/2请求
- 保持对HTTP/1.1客户端的兼容（可通过`MYFRAME_SSL_ALPN=h2`仅启用h2）
- 演示大响应体/流控（`/big`）

**运行方式:**
```bash
./h2_server [端口]
```

**测试命令:**
```bash
curl -k --http2 https://127.0.0.1:7779/hello
curl -k --http2 https://127.0.0.1:7779/big
./h2_client h2://127.0.0.1:7779/hello
```

### 4. WebSocket安全服务器 (`simple_wss_server.cpp`)
WSS (WebSocket over SSL)服务器示例，展示如何：
- 创建安全的WebSocket连接
- 处理WebSocket消息
- 实现双向通信

**运行方式:**
```bash
./wss_server [端口]
```

**测试命令:**
```bash
websocat wss://127.0.0.1:7778/websocket
```

### 5. 多协议服务器 (`multi_protocol_server.cpp`)
同端口多协议服务器示例，展示如何：
- 在同一端口支持多种协议(HTTP/HTTPS/WS/WSS)
- 自动协议检测和切换
- 统一的业务逻辑处理

**运行方式:**
```bash
./multi_protocol_server [端口] [--tls-only]
```

**测试命令:**
HTTP:
```bash
curl http://127.0.0.1:7782/hello
```

HTTPS:
```bash
curl -k https://127.0.0.1:7782/hello
```

WebSocket:
```bash
websocat ws://127.0.0.1:7782/websocket
```

WSS:
```bash
websocat wss://127.0.0.1:7782/websocket
```

## 客户端示例

### 1. HTTP客户端 (`simple_http_client.cpp`)
基础HTTP客户端示例，展示如何：
- 发送HTTP请求
- 处理HTTP响应
- 连接到HTTP服务器

### 2. HTTPS客户端 (`simple_https_client.cpp`)
HTTPS安全客户端示例，展示如何：
- 建立SSL/TLS连接
- 发送HTTPS请求
- 处理安全连接

### 3. HTTP/2 客户端 (`simple_h2_client.cpp`)
基于`client_iface`的HTTP/2客户端示例，展示如何：
- 使用`make_http2_factory()`创建HTTP/2请求
- 支持`https://`（ALPN协商h2）或非标准`h2://`（强制HTTP/2）URL

**使用示例:**
```bash
./h2_client h2://127.0.0.1:7779/hello
./h2_client https://127.0.0.1:7779/hello
```

### 4. WebSocket客户端 (`simple_ws_client.cpp`)
WebSocket客户端示例，展示如何：
- 建立WebSocket连接
- 发送WebSocket升级请求
- 测试协议检测功能

## 编译说明

### 使用CMake编译所有示例:
```bash
cd /home/rong/myframe
mkdir -p build && cd build
cmake ..
make examples
```

### 编译后的可执行文件位置:
```bash
build/examples/http_server
build/examples/https_server
build/examples/h2_server
build/examples/wss_server
build/examples/multi_protocol_server
build/examples/http_client
build/examples/https_client
build/examples/h2_client
build/examples/ws_client
```

## 框架特性演示

这些示例展示了MyFrame框架的核心特性：

1. **多协议支持**: 同时支持HTTP、HTTPS、WebSocket、WSS
2. **协议自动检测**: 可在同一端口自动识别不同协议
3. **SSL/TLS加密**: 内置安全传输层支持
4. **统一接口**: 统一的业务逻辑处理接口
5. **高性能**: 基于epoll的高性能网络处理
6. **灵活配置**: 支持多种运行模式和配置选项

## 依赖要求

- C++11或更高版本
- OpenSSL库
- pthread库
- CMake 3.10或更高版本

## 注意事项

- HTTPS和WSS需要SSL证书文件，示例使用项目中的测试证书
- 某些示例需要先构建框架的依赖库(encode, sign)
- 建议先运行`scripts/build.sh`准备依赖库
