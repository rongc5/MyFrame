# MyFrame 新增示例程序

本文档介绍两个新增的示例程序，展示MyFrame框架的高级特性。

## 示例程序

### 1. demo_multi_protocol_server - 多协议服务器

**功能特点：**
- 同一端口同时支持HTTP和WebSocket协议
- 自动协议检测和路由
- 实时连接统计
- RESTful API接口

**启动方法：**
```bash
# 编译
./scripts/build_cmake.sh all

# 运行（默认端口8888）
./build/examples/demo_multi_protocol_server

# 指定端口
./build/examples/demo_multi_protocol_server 8080
```

**测试方法：**
```bash
# HTTP接口测试
curl http://127.0.0.1:8888/
curl http://127.0.0.1:8888/api/status
curl http://127.0.0.1:8888/api/stats
curl http://127.0.0.1:8888/api/time

# WebSocket测试（需要websocat工具）
websocat ws://127.0.0.1:8888/ws
# 发送消息：ping, stats, time, 或任意文本

# 浏览器测试
# 打开 http://127.0.0.1:8888/ 查看测试页面
```

**架构说明：**
- 使用MultiProtocolFactory实现协议自动检测
- 单个IAppHandler处理HTTP和WebSocket请求
- 展示了同一端口多协议的实现方式

### 2. demo_multi_thread_server - 多线程服务器

**功能特点：**
- 1个监听线程 + 多个工作线程架构
- 负载均衡和线程统计
- 高并发请求处理
- 性能测试接口

**启动方法：**
```bash
# 运行（默认端口9999，4个线程）
./build/examples/demo_multi_thread_server

# 指定端口和线程数
./build/examples/demo_multi_thread_server 9999 6
```

**测试方法：**
```bash
# 查看线程信息
curl http://127.0.0.1:9999/api/thread-info

# 查看线程统计
curl http://127.0.0.1:9999/api/thread-stats

# 负载测试（使用ab工具）
ab -n 1000 -c 10 http://127.0.0.1:9999/api/load-test

# 重置统计
curl -X POST http://127.0.0.1:9999/api/reset-stats

# 浏览器测试
# 打开 http://127.0.0.1:9999/ 查看测试页面
```

**架构说明：**
- 使用server类的多线程构造函数
- 展示了线程池负载均衡
- 每个工作线程维护独立的统计信息
- 适合高并发场景

## 编译和部署

### 编译要求
- C++11或更高版本
- CMake 3.16+
- OpenSSL开发库
- pthread支持

### 编译步骤
```bash
# 清理构建
./scripts/build_cmake.sh clean

# 构建所有示例
./scripts/build_cmake.sh all

# 仅构建框架库
./scripts/build_cmake.sh
```

### 输出文件
编译成功后，可执行文件位于：
- `build/examples/demo_multi_protocol_server`
- `build/examples/demo_multi_thread_server`

## 框架特性展示

### 协议支持
- HTTP/1.1
- WebSocket
- 自定义协议（通过扩展）

### 线程模型
- 单线程模式（适合轻量级应用）
- 多线程模式（1个监听 + N个工作线程）
- 线程池负载均衡

### 高级特性
- 协议自动检测
- 连接复用
- 统计监控
- 优雅关闭

## 性能测试

### 多协议服务器测试
```bash
# HTTP并发测试
ab -n 10000 -c 100 http://127.0.0.1:8888/api/status

# WebSocket连接测试
for i in {1..10}; do websocat ws://127.0.0.1:8888/ws &; done
```

### 多线程服务器测试
```bash
# 高并发负载测试
ab -n 50000 -c 200 http://127.0.0.1:9999/api/load-test

# 查看线程分布
while true; do curl -s http://127.0.0.1:9999/api/thread-stats | jq; sleep 2; done
```

## 扩展开发

### 添加新协议
1. 继承IAppHandler接口
2. 实现on_custom()方法
3. 在MultiProtocolFactory中注册

### 自定义线程策略
1. 继承base_net_thread类
2. 重写线程调度逻辑
3. 配置server使用自定义线程

### 监控集成
- 通过/api/stats接口获取实时统计
- 支持Prometheus格式输出
- 可集成到监控系统

## 故障排除

### 常见问题
1. **端口占用**：使用不同端口或检查现有进程
2. **编译错误**：确保依赖库正确安装
3. **连接失败**：检查防火墙设置

### 调试方法
```bash
# 启用详细日志
export MYFRAME_LOG_LEVEL=debug

# 使用gdb调试
gdb ./build/examples/demo_multi_protocol_server

# 查看网络连接
netstat -tlnp | grep :8888
```

## 参考资料

- [MyFrame框架文档](../README.md)
- [核心API文档](../core/README.md)
- [更多示例](../examples/)

---
*MyFrame Framework - 高性能C++网络框架*
