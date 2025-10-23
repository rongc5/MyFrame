# MyFrame 股票系统示例

这是一个完整的股票系统演示，展示了MyFrame框架的分布式服务架构。

## 系统架构

### 服务组件

1. **stock_index_server** (端口7801)
   - 股票索引服务
   - 提供股票符号列表
   - 支持HTTP API和WebSocket

2. **stock_bridge_server** (端口7802)
   - 股票桥接服务
   - 处理路由和转发
   - 支持HTTP API和WebSocket

3. **stock_driver** (端口7800)
   - 驱动程序和协调器
   - 连接到index和bridge服务
   - 提供统一的状态监控

### 通信架构
```
[Client] --> [stock_driver:7800] --> [stock_index_server:7801]
                    |
                    └--> [stock_bridge_server:7802]
```

## 使用方法

### 快速启动
```bash
# 编译所有组件
./scripts/build_cmake.sh all

# 运行演示脚本（自动启动所有服务）
./examples/stock_system_demo.sh
```

### 手动启动
```bash
cd build/examples

# 启动股票索引服务
./stock_index_server 7801 &

# 启动股票桥接服务  
./stock_bridge_server 7802 &

# 启动驱动程序
./stock_driver 7800 &
```

### API测试

#### 股票索引服务 (7801)
```bash
# 服务状态
curl http://127.0.0.1:7801/api/status

# 股票符号列表
curl http://127.0.0.1:7801/symbols

# WebSocket测试
websocat ws://127.0.0.1:7801/websocket
# 发送: ping, symbols, hello
```

#### 股票桥接服务 (7802)
```bash
# 服务状态
curl http://127.0.0.1:7802/api/status

# WebSocket测试
websocat ws://127.0.0.1:7802/websocket
# 发送: ping, route:AAPL, hello
```

#### 驱动程序 (7800)
```bash
# 服务状态和连接状态
curl http://127.0.0.1:7800/api/status

# WebSocket测试
websocat ws://127.0.0.1:7800/websocket
# 发送: ping, status, hello
```

## 技术特点

### 框架使用
- **MultiProtocolFactory**: 同一端口支持HTTP和WebSocket
- **ListenFactory**: 正确的监听管理，避免端口重复绑定
- **WsSyncClient**: 同步WebSocket客户端，用于服务间通信
- **server**: 多线程架构，1个监听线程+1个工作线程

### 修复的问题
原始代码存在的问题：
```cpp
// 错误的使用方式 - 会导致端口重复绑定
server srv(2);
srv.bind("0.0.0.0", port);  // 直接绑定
srv.set_business_factory(biz); // 设置业务工厂
```

修复后的正确使用方式：
```cpp
// 正确的使用方式 - 使用ListenFactory管理监听
server srv(2);
auto lsn = std::make_shared<ListenFactory>("0.0.0.0", port, biz);
srv.set_business_factory(lsn); // 使用ListenFactory包装
```

### 服务发现和健康检查
- 驱动程序自动连接到其他服务
- 连接状态实时监控
- 自动重连机制
- 统计信息收集

## 扩展开发

### 添加新的股票服务
1. 继承 `IAppHandler` 接口
2. 实现 `on_http()` 和 `on_ws()` 方法
3. 使用 `ListenFactory` 和 `MultiProtocolFactory`
4. 在驱动程序中添加连接逻辑

### 示例代码框架
```cpp
#include "server.h"
#include "multi_protocol_factory.h"
#include "listen_factory.h"
#include "app_handler.h"

class MyStockHandler : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        // 处理HTTP请求
    }
    
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        // 处理WebSocket消息
    }
};

int main() {
    auto handler = std::make_shared<MyStockHandler>();
    auto biz = std::make_shared<MultiProtocolFactory>(
        handler.get(), MultiProtocolFactory::Mode::PlainOnly);
    
    server srv(2);
    auto lsn = std::make_shared<ListenFactory>("0.0.0.0", port, biz);
    srv.set_business_factory(lsn);
    
    srv.start();
    srv.join();
    return 0;
}
```

## 性能测试

### 负载测试
```bash
# HTTP负载测试
ab -n 10000 -c 100 http://127.0.0.1:7801/api/status

# WebSocket并发连接测试
for i in {1..50}; do 
    websocat ws://127.0.0.1:7801/websocket &
done
```

### 监控指标
- 连接数统计
- 请求响应时间
- 服务间通信延迟
- 错误率监控

## 故障排除

### 常见问题
1. **端口占用**: 检查端口是否被其他程序占用
2. **连接失败**: 确保服务启动顺序正确
3. **编译错误**: 检查依赖库是否正确安装

### 调试方法
```bash
# 检查端口状态
netstat -tlnp | grep -E ":(7800|7801|7802)"

# 查看进程状态
ps aux | grep stock_

# 查看日志输出
./stock_system_demo.sh 2>&1 | tee stock_system.log
```

## 部署建议

### 生产环境
- 使用进程管理器 (systemd, supervisor)
- 配置日志轮转
- 添加健康检查端点
- 实现优雅关闭

### 容器化
```dockerfile
FROM ubuntu:20.04
COPY build/examples/stock_* /app/
EXPOSE 7800 7801 7802
CMD ["/app/stock_system_demo.sh"]
```

---
*MyFrame Framework - 高性能C++网络框架股票系统示例*
