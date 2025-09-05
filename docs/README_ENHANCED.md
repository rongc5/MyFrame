# MyFrame Enhanced Framework

## 概述

这是基于原有MyFrame框架的增强版本，集成了以下新功能：

## 新增功能

### 1. Server类封装
- **server**: 功能完整的服务器类，支持工厂模式和自动协议检测。
- **ListenFactory**: 可插入到 `server.set_business_factory()` 的监听包装器，负责创建 `listen_fd` 并把新连接轮询分发给 worker 线程。
- 默认线程数=2：1个监听线程 + 1个工作线程；可通过构造参数调整。

### 2. SSL/HTTPS支持  
- **ssl_context**: SSL上下文管理
- **ssl_config**: SSL配置管理
- 支持TLS 1.2/1.3协议
- 支持自定义证书和密钥配置
  - 服务器默认从`tests/common/cert/server.crt`和`server.key`加载（开发环境）
  - 可通过环境变量覆盖：`MYFRAME_SSL_CERT`、`MYFRAME_SSL_KEY`
  
#### HTTPS/WSS 快速体验
1) 生成测试证书：
```
cd tests/common/cert && ./make_cert.sh
```
2) 启动示例服务器（多协议自动检测）：
```
cd test && ./test_server_simple   # 监听 127.0.0.1:7777
```
3) 验证客户端：
```
./test/simple_https_client 7777
./test/simple_wss_client 7777
```

### 3. HTTP/WebSocket客户端（含HTTPS/WSS）
- **http_client**: 支持HTTP/1.1 GET/POST；支持HTTPS：
  - 端口为443或设置环境变量`MYFRAME_HTTP_SSL=1`时自动启用TLS
  - 新增重载：`get(..., bool use_ssl, ...)`、`post(..., bool use_ssl, ...)`
  - 证书校验：`MYFRAME_SSL_VERIFY=1`启用证书校验（默认关闭，适配自签证书）
- **ws_client**: 支持WebSocket；支持WSS：
  - 端口为443或设置`MYFRAME_WS_SSL=1`时自动启用TLS
  - 新增重载：`connect_and_send(..., bool use_ssl, ...)`

#### 3.1 ClientFactory（统一工厂，按URL自动选择协议）
- 头文件：`client_factory.h`（已作为公共头暴露）
- 核心接口：
  - `IRequestClient`：请求-响应语义（HTTP/HTTPS）
  - `IStreamClient`：会话/双工语义（WS/WSS）
  - `ClientFactory::create(url)`：根据`scheme`返回具体客户端适配器

使用示例：
```cpp
#include "client_factory.h"

int main() {
    // HTTP/HTTPS（请求-响应）
    auto c1 = ClientFactory::create("https://127.0.0.1:7777/api");
    if (auto* http = dynamic_cast<IRequestClient*>(c1.get())) {
        std::string body;
        http->get("/api", body, 3000);
    }

    // WS/WSS（会话/双工）
    auto c2 = ClientFactory::create("wss://127.0.0.1:7777/ws");
    if (auto* ws = dynamic_cast<IStreamClient*>(c2.get())) {
        std::string reply;
        ws->connect_and_send("/ws", "hello", reply, 3000);
    }
}
```

扩展新协议：
```cpp
// 注册自定义scheme，例如 grpc://
ClientFactory::register_scheme("grpc", [](const client_url& u){
    // 返回实现 IClient 的自定义适配器
    return std::unique_ptr<IClient>(new GrpcClientAdapter(u));
});
```

### 4. 协议自动检测
- **detect_process**: 自动检测HTTP/WebSocket协议
- 支持同一端口处理多种协议
- 智能路由到相应的处理器

### 5. 线程池管理
- 监听线程维护 worker 线程池索引，`add_worker_thread()` 注册；`next_worker_rr()` 轮询选择 worker。
- `listen_process` 优先使用监听线程的线程池进行分发；同时兼容旧的 `on_accept` 路径。

## 构建方式

### 传统Makefile构建
```bash
# 使用原有构建脚本
./build.sh

# 或使用新的构建脚本
./scripts/build.sh
```

### CMake构建
```bash
# 默认Debug，仅构建core库（目标 myframe）
./scripts/build_cmake.sh

# Release构建
./scripts/build_cmake.sh release

# 构建所有目标（examples/tests）
./scripts/build_cmake.sh all

# 清理构建
./scripts/build_cmake.sh clean
```

## 测试程序

### 服务器测试
```bash
cd test
make test_server_simple
./test_server_simple
```

### HTTP客户端测试
```bash
cd test  
make test_http_client
./test_http_client
```

## 文件结构

```
myframe/
├── core/                 # 核心框架代码
│   ├── server.cpp/.h     # 完整服务器类
│   ├── server_simple.cpp/.h  # 简化服务器类
│   ├── ssl_context.cpp/.h     # SSL上下文
│   ├── ssl_config.h      # SSL配置
│   ├── http_client.cpp/.h     # HTTP客户端
│   ├── ws_client.cpp/.h  # WebSocket客户端
│   ├── detect_process.cpp/.h  # 协议检测
│   └── net_thread_pool.cpp/.h # 线程池
├── scripts/              # 构建脚本
│   ├── build.sh          # 传统构建脚本
│   └── build_cmake.sh    # CMake构建脚本
├── test/                 # 测试程序
│   ├── test_server_simple.cpp  # 服务器测试
│   └── test_http_client.cpp    # HTTP客户端测试
├── CMakeLists.txt        # CMake配置
└── README_ENHANCED.md    # 本文档
```

## 兼容性

- 保持与原有MyFrame框架的API兼容性
- 新功能作为扩展，不影响现有代码
- 支持C++11标准

## 备份

原始框架已备份到：
- `backup_original/` 目录

## SSL证书配置示例

```cpp
#include "ssl_context.h"

ssl_config config;
config._cert_file = "server.crt";
config._key_file = "server.key"; 
config._protocols = "TLSv1.2,TLSv1.3";
config._verify_peer = false;

ssl_context ctx;
ctx.init_server(config);
```

## 服务器使用示例（ListenFactory新用法）

```cpp
#include "server.h"
#include "multi_protocol_factory.h"
#include "../core/listen_factory.h"

server srv; // 默认2线程：1 listen + 1 worker

auto app = std::make_shared<MyHandler>();
auto biz = std::make_shared<MultiProtocolFactory>(app.get()); // 或 Mode::TlsOnly
auto lsn = std::make_shared<ListenFactory>("0.0.0.0", 8080, biz);

srv.set_business_factory(lsn);
srv.start();
```

说明：
- `ListenFactory` 会在监听线程初始化时创建并注册 `listen_fd`（ET模式），`accept` 后将新连接封装为 `NORMAL_MSG_CONNECT` 交给 worker。
- 如仍想走旧路径，可直接 `server.set_business_factory(biz); server.bind(ip,port);`，兼容保留。

## 编译要求

- GCC 4.9+ 或等效编译器
- OpenSSL开发库 (对于SSL功能)
- pthread库
- C++11支持

## 贡献指南

- 贡献前请阅读 `AGENTS.md`（Repository Guidelines），了解项目结构、构建/测试命令、代码风格与PR要求。
- 提交PR前请运行：`cd tests && bash run_all.sh`，确保用例通过并与CI一致（见 `.github/workflows/test.yml`）。
- 也可参见 `CONTRIBUTING.md` 获取简要流程与检查清单。

## 测试修正规划（协议覆盖）

为系统性验证框架对 HTTP/HTTPS/WS/WSS 的支持（单协议与同端口多协议，服务端与客户端互通），请参见：

- `docs/TESTING_PLAN_CN.md`

文档包含：前置准备、单协议与多协议用例、客户端独立验证、负面边界、关键日志断言点以及自动化落地建议（tests/case*/ + run_all.sh）。

## 注意事项

1. SSL功能需要在编译时定义`ENABLE_SSL`宏
2. 需要链接OpenSSL库 (`-lssl -lcrypto`)
3. 确保有足够的权限监听指定端口
