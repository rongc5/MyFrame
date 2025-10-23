# 多协议框架测试方案

## 测试完成状态

### ✅ 已完成
- **核心重构**: 4个关键隐患修复完成
- **单元测试**: IAppHandler、Codec、protocol_detect_process、ConnectionFactory
- **编译验证**: 框架编译通过，.o文件正确放置
- **业务层抽象**: 实现"业务0感知协议"目标

### 🔄 集成测试阶段

#### 手动测试步骤：

1. **启动测试服务器**
```bash
cd /home/rong/myframe/test
./test_integration_server 8080
```

2. **HTTP测试**
```bash
curl http://127.0.0.1:8080/hello
curl http://127.0.0.1:8080/api/status
```

3. **WebSocket测试**
```bash
# 需要安装wscat: npm install -g wscat
wscat -c ws://127.0.0.1:8080/chat
# 发送: ping
# 期望回复: pong
```

4. **并发测试**
```bash
# HTTP负载测试
wrk -t4 -c100 -d10s http://127.0.0.1:8080/hello

# 或使用ab
ab -n 1000 -c 10 http://127.0.0.1:8080/hello
```

#### 测试预期结果：
- HTTP请求返回200状态码和正确内容
- WebSocket连接成功建立并能双向通信
- 同一端口同时支持HTTP和WebSocket协议
- 业务处理器收到协议无关的请求对象

### 📋 待测试项目

#### HTTPS/WSS支持
```bash
# 生成测试证书
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes

# HTTPS测试
curl -k https://127.0.0.1:8443/hello

# WSS测试  
wscat -n --no-check -c wss://127.0.0.1:8443/chat
```

#### 压力测试
- 1000并发HTTP请求稳定性
- 1000个WebSocket连接同时收发消息
- 长连接保持测试（24小时）
- 内存泄漏检测（valgrind）

#### 功能验证
- [ ] 协议自动检测（HTTP→WebSocket升级）
- [ ] TLS握手和加密通信
- [ ] 跨线程消息推送
- [ ] 热更新证书
- [ ] 定时器功能
- [ ] 异常恢复

## 框架核心特性

### 业务层抽象
```cpp
class MyBusinessHandler : public IAppHandler {
    void on_http(const HttpRequest& req, HttpResponse& rsp) override;
    void on_ws(const WsFrame& recv, WsFrame& send) override;
};
```

### 统一连接创建
```cpp
auto conn = ConnectionFactory::create_multi_protocol_connection(sock, &handler);
// 自动支持 HTTP/WebSocket/TLS 协议检测
```

### 协议透明
- 业务代码完全协议无关
- 同一handler处理HTTP和WebSocket
- 自动协议检测和切换
- 支持TLS加密层

## 下一步计划

1. **完善集成测试**: 实现完整的服务器集成测试
2. **性能基线**: 建立性能基准和回归测试
3. **生产部署**: 文档化部署和运维指南
4. **监控告警**: 添加关键指标监控

---
*🤖 Generated with Claude Code*