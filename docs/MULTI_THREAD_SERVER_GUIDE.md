# MyFrame 多线程服务器示例

## 概述

`multi_thread_server` 是一个展示 MyFrame 框架多线程架构的完整示例，演示了：
- **1个监听线程** 接收连接
- **多个工作线程** 处理请求 
- **多协议支持** (HTTP + WebSocket)
- **负载均衡** 和线程统计

## 编译和运行

### 编译
```bash
# 在项目根目录执行
./scripts/build_cmake.sh all
```

### 可执行文件位置
```
/home/rong/myframe/build/examples/demo_multi_thread_server
```

### 运行命令
```bash
cd /home/rong/myframe/build/examples
./demo_multi_thread_server [端口] [线程数]

# 示例
./demo_multi_thread_server 9998 4    # 端口9998，4线程(1监听+3工作)
./demo_multi_thread_server 8080      # 端口8080，默认4线程
./demo_multi_thread_server            # 默认端口9999，默认4线程
```

### 注意事项
- 如果遇到端口占用错误，更换端口号
- 控制台中文可能显示为乱码，但不影响功能
- 程序已修复中文编码问题（源码 line 75-82）

## 架构说明

### 线程模型
- **监听线程 (1个)**: 接收新连接，负责 bind/listen/accept
- **工作线程 (N-1个)**: 处理具体的HTTP/WebSocket请求
- **统计线程 (1个)**: 每10秒打印线程统计信息

### 协议支持
1. **HTTP协议**: RESTful API，支持GET/POST等
2. **WebSocket协议**: 实时通信，支持ping/pong等

### 负载均衡
- 使用 Round-Robin 算法将连接分发到工作线程
- 每个工作线程维护独立的统计计数器

## API接口

### HTTP接口

#### 1. 主页
```bash
curl http://127.0.0.1:9999/
```
返回HTML页面，包含所有测试链接

#### 2. 线程信息
```bash
curl http://127.0.0.1:9999/api/thread-info
```
返回当前处理线程的信息：
```json
{
  "thread_count": 8,
  "current_thread": "140123456789",
  "request_id": 123
}
```

#### 3. 统计信息
```bash
curl http://127.0.0.1:9999/api/thread-stats
```
返回所有线程的统计信息：
```json
{
  "total_requests": 1250,
  "total_connections": 15,
  "total_messages": 50,
  "threads": [
    "Thread-140123456789: Req=320, Conn=4, Msg=12",
    "Thread-140123456790: Req=315, Conn=3, Msg=15"
  ]
}
```

#### 4. 负载测试接口
```bash
curl http://127.0.0.1:9999/api/load-test
```
模拟延迟，返回请求信息：
```json
{
  "request_id": 456,
  "thread_id": "140123456789",
  "status": "ok",
  "timestamp": 1694444444123
}
```

#### 5. 重置统计
```bash
curl -X POST http://127.0.0.1:9999/api/reset-stats
```

### WebSocket接口

#### 连接端点
```
ws://127.0.0.1:9999/ws
```

#### 特殊命令
- 发送 `ping` → 回复 `pong from thread [ID]`
- 发送 `thread-info` → 回复线程统计信息
- 发送其他消息 → Echo回复 `[Thread-ID] Echo: [消息]`

## 实际测试结果

### 启动服务器
```bash
cd /home/rong/myframe/build/examples
./demo_multi_thread_server 9998 4 &
```

**预期输出**:
```
=== MyFrame 多线程服务器 ===
端口: 9998
线程数: 4 (1个监听 + 3个工作)
CPU核心数: 2
[listen] bound 127.0.0.1:9998, fd=13
[listen] added listen_fd=13 to epoll
✓ 多线程服务器启动成功!
```

## 测试方法

### 1. 基本功能测试
```bash
# 测试线程信息接口
curl -s http://127.0.0.1:9998/api/thread-info | jq .

# 预期响应
{
  "thread_count": 2,
  "current_thread": "250500932956512", 
  "request_id": 1
}

# 测试统计信息接口
curl -s http://127.0.0.1:9998/api/thread-stats | jq .

# 预期响应
{
  "total_requests": 2,
  "total_connections": 0,
  "total_messages": 0,
  "threads": [
    "Thread-250500932956512: Req=1, Conn=0, Msg=0",
    "Thread-250500924502368: Req=1, Conn=0, Msg=0"
  ]
}
```

### 2. 负载均衡验证测试
```bash
# 发送10个并发请求测试负载均衡
for i in {1..10}; do 
  curl -s http://127.0.0.1:9998/api/load-test > /dev/null & 
done
wait

# 查看请求分布
curl -s http://127.0.0.1:9998/api/thread-stats | jq .

# 预期看到请求均匀分布到不同线程
{
  "total_requests": 12,
  "total_connections": 0, 
  "total_messages": 0,
  "threads": [
    "Thread-250500932956512: Req=4, Conn=0, Msg=0",
    "Thread-250500924502368: Req=4, Conn=0, Msg=0", 
    "Thread-250500916048224: Req=4, Conn=0, Msg=0"
  ]
}
```

### 3. 服务器日志观察
**控制台输出示例**:
```
[HTTP-250500932956512] 请求#1 GET /api/thread-info
[accept] epoll in
[factory] NORMAL_MSG_CONNECT received
[factory] creating connection for fd=16
[factory] connection enqueued to container thread=1
[detect] HttpProbe selected
[HTTP-250500924502368] 请求#2 GET /api/thread-stats

=== 线程统计 (每10秒) ===
总请求: 12, 总连接: 0, 总消息: 0
  Thread-250500932956512: Req=4, Conn=0, Msg=0
  Thread-250500924502368: Req=4, Conn=0, Msg=0
  Thread-250500916048224: Req=4, Conn=0, Msg=0
=========================
```

### 4. 高并发压力测试
```bash
# Apache Bench 压力测试
ab -n 1000 -c 10 http://127.0.0.1:9998/api/load-test

# 预期结果：观察到请求被均匀分发到不同工作线程
curl -s http://127.0.0.1:9998/api/thread-stats | jq '.threads'
```

### 5. WebSocket测试
```bash
# 使用websocat测试WebSocket（需要安装websocat）
websocat ws://127.0.0.1:9998/ws

# 发送测试消息：
ping              # 应回复: pong from thread [ID]
thread-info       # 应回复: Thread: [ID], Stats: ...
hello world       # 应回复: [Thread-ID] Echo: hello world
```

### 6. 浏览器测试
```bash
# 在浏览器访问
http://127.0.0.1:9998/

# 页面显示完整的测试链接和说明
```

## 验证多线程负载均衡

### 关键验证点

1. **线程创建验证**:
   ```bash
   # 启动时应看到多个线程创建日志
   tid:[250500944322592] ... add to epoll _epoll_fd[3] _get_sock [4]    # 监听线程
   tid:[250500944322592] ... add to epoll _epoll_fd[6] _get_sock [7]    # 工作线程1  
   tid:[250500944322592] ... add to epoll _epoll_fd[9] _get_sock [10]   # 工作线程2
   tid:[250500944322592] ... add to epoll _epoll_fd[12] _get_sock [13]  # 工作线程3
   ```

2. **请求分发验证**:
   ```bash
   # 观察不同线程ID处理请求
   [HTTP-250500932956512] 请求#1 GET /api/thread-info    # 线程A处理
   [HTTP-250500924502368] 请求#2 GET /api/thread-stats   # 线程B处理
   [HTTP-250500916048224] 请求#3 GET /api/load-test      # 线程C处理
   ```

3. **负载均衡效果验证**:
   ```bash
   # 10个请求后统计应显示接近均匀分布
   "threads": [
     "Thread-250500932956512: Req=3, Conn=0, Msg=0",  # ~33%
     "Thread-250500924502368: Req=4, Conn=0, Msg=0",  # ~40% 
     "Thread-250500916048224: Req=3, Conn=0, Msg=0"   # ~33%
   ]
   ```

## 监控和调试

### 实时监控脚本
```bash
#!/bin/bash
# 监控服务器负载分布
echo "监控多线程服务器负载分布..."
while true; do
    STATS=$(curl -s http://127.0.0.1:9998/api/thread-stats 2>/dev/null)
    if [ $? -eq 0 ]; then
        TOTAL=$(echo $STATS | jq -r '.total_requests')
        echo "$(date '+%H:%M:%S') - 总请求: $TOTAL"
        echo $STATS | jq -r '.threads[]' | sed 's/^/  /'
        echo "---"
    fi
    sleep 3
done
```

### 性能基准测试
```bash
# 重置统计
curl -X POST http://127.0.0.1:9998/api/reset-stats

# 执行基准测试
ab -n 1000 -c 10 http://127.0.0.1:9998/api/load-test

# 查看最终分布
curl -s http://127.0.0.1:9998/api/thread-stats | jq '.threads'
```

## 自动化测试脚本

### 使用测试脚本
```bash
# 运行自动化测试
/home/rong/myframe/docs/scripts/test_multi_thread_server.sh [端口] [线程数]

# 示例
cd /home/rong/myframe/docs/scripts
./test_multi_thread_server.sh 9998 4
```

**测试脚本功能**:
- ✅ 基本API功能测试  
- ✅ 并发负载均衡验证
- ✅ Apache Bench 压力测试
- ✅ 响应时间分析
- ✅ 最终测试报告

## 常见问题排查

### 1. 服务器启动失败
```bash
# 错误：bind error 127.0.0.1:9999
# 解决：端口被占用，更换端口
netstat -tlnp | grep 9999
kill -9 <pid>

# 或直接使用其他端口
./demo_multi_thread_server 9998 4
```

### 2. 中文乱码问题  
```bash
# 现象：控制台显示乱码字符
# 影响：仅影响显示，不影响功能
# 解决：已在源码中添加UTF-8支持 (line 75-82)

# 临时解决方案
export LANG=zh_CN.UTF-8
export LC_ALL=zh_CN.UTF-8
```

### 3. 请求超时或连接失败
```bash
# 检查服务器状态
curl -v http://127.0.0.1:9998/api/thread-info

# 检查防火墙
sudo ufw status
sudo ufw allow 9998

# 检查进程
ps aux | grep demo_multi_thread_server
```

### 4. 负载不均衡
```bash
# 正常情况：请求应均匀分布到各工作线程
# 异常情况：所有请求都在一个线程处理

# 检查线程数配置
curl -s http://127.0.0.1:9998/api/thread-stats | jq '.threads | length'

# 验证多次请求的线程分布
for i in {1..10}; do 
  curl -s http://127.0.0.1:9998/api/load-test > /dev/null
done
curl -s http://127.0.0.1:9998/api/thread-stats | jq '.threads'
```

### 5. JSON 解析错误
```bash
# 检查响应格式
curl -v http://127.0.0.1:9998/api/thread-info

# 确保 Content-Type 正确
# 应该显示: Content-Type: application/json
```

## 停止服务器
```bash
# 方法1：使用Ctrl+C优雅停止
# 方法2：发送信号
kill -INT <pid>

# 方法3：强制终止所有实例  
pkill -f demo_multi_thread_server
```

## 架构优势

1. **高并发**: 多线程处理，充分利用多核CPU
2. **协议无关**: 同端口支持HTTP和WebSocket
3. **负载均衡**: 自动分发请求到不同工作线程
4. **实时监控**: 内置统计和监控功能
5. **易扩展**: 基于框架的插件式架构

## 相关文件

- 源码: `/home/rong/myframe/examples/multi_thread_server.cpp`
- 可执行文件: `/home/rong/myframe/build/examples/demo_multi_thread_server`
- 编译脚本: `/home/rong/myframe/scripts/build_cmake.sh`