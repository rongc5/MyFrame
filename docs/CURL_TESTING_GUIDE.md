# MyFrame 服务器 cURL 测试指南

## 快速开始

### 启动服务器
```bash
cd /home/rong/myframe/build/examples
./demo_multi_thread_server 9998 4  # 使用端口9998避免冲突
```

**预期启动输出**:
```
=== MyFrame 多线程服务器 ===  
端口: 9998
线程数: 4 (1个监听 + 3个工作)
CPU核心数: 2
[listen] bound 127.0.0.1:9998, fd=13
✓ 多线程服务器启动成功!
```

## HTTP 接口测试

### 1. 基础接口测试
```bash
# 线程信息 - JSON响应  
curl -s http://127.0.0.1:9998/api/thread-info | jq .

# 预期响应示例:
{
  "thread_count": 2,
  "current_thread": "250500932956512", 
  "request_id": 1
}

# 统计信息 - JSON响应
curl -s http://127.0.0.1:9998/api/thread-stats | jq .

# 预期响应示例:
{
  "total_requests": 2,
  "total_connections": 0,
  "total_messages": 0,
  "threads": [
    "Thread-250500932956512: Req=1, Conn=0, Msg=0",
    "Thread-250500924502368: Req=1, Conn=0, Msg=0"
  ]
}

# 负载测试接口
curl -s http://127.0.0.1:9998/api/load-test | jq .

# 主页 - HTML响应
curl http://127.0.0.1:9998/
```

### 2. 详细响应查看
```bash
# 查看完整HTTP头和响应
curl -v http://127.0.0.1:9999/api/thread-info

# 只看HTTP头
curl -I http://127.0.0.1:9999/api/thread-stats

# 格式化JSON输出(需要jq)
curl -s http://127.0.0.1:9999/api/thread-stats | jq .
```

### 3. POST请求测试
```bash
# 重置统计信息
curl -X POST http://127.0.0.1:9999/api/reset-stats

# 查看重置后的统计
curl http://127.0.0.1:9999/api/thread-stats
```

## 并发测试

### 1. 简单并发测试（验证负载均衡）
```bash
# 重置统计
curl -X POST http://127.0.0.1:9998/api/reset-stats

# 10个并发请求测试负载均衡
for i in {1..10}; do
  curl -s http://127.0.0.1:9998/api/load-test > /dev/null &
done
wait

# 查看并发处理结果和负载分布
curl -s http://127.0.0.1:9998/api/thread-stats | jq .

# 预期结果：请求应均匀分布到不同工作线程
# 示例输出：
{
  "total_requests": 10,
  "total_connections": 0,
  "total_messages": 0,
  "threads": [
    "Thread-250500932956512: Req=3, Conn=0, Msg=0",  # 线程1: 3个请求
    "Thread-250500924502368: Req=4, Conn=0, Msg=0",  # 线程2: 4个请求  
    "Thread-250500916048224: Req=3, Conn=0, Msg=0"   # 线程3: 3个请求
  ]
}
```

### 2. 高并发压测
```bash
# 100个并发请求，观察线程负载均衡
for i in {1..100}; do
  curl -s http://127.0.0.1:9999/api/load-test > /dev/null &
done
wait

# 查看负载分布
curl -s http://127.0.0.1:9999/api/thread-stats | jq .
```

### 3. 持续压测
```bash
# 持续5秒的并发请求
timeout 5s bash -c '
while true; do
  curl -s http://127.0.0.1:9999/api/load-test > /dev/null &
  sleep 0.01
done
'

# 等待完成并查看统计
sleep 2
curl http://127.0.0.1:9999/api/thread-stats
```

## 性能测试工具

### Apache Bench (ab)
```bash
# 基础压测: 1000请求，10并发
ab -n 1000 -c 10 http://127.0.0.1:9999/api/load-test

# 长时间压测: 30秒，10并发
ab -t 30 -c 10 http://127.0.0.1:9999/api/load-test

# 查看详细统计
ab -n 100 -c 5 -v 2 http://127.0.0.1:9999/api/load-test
```

### wrk (推荐)
```bash
# 如果有wrk工具
wrk -t4 -c12 -d30s http://127.0.0.1:9999/api/load-test
```

## 测试脚本示例

### 自动化测试脚本
```bash
#!/bin/bash
# 保存为 test_multi_thread_server.sh

SERVER_URL="http://127.0.0.1:9999"

echo "=== MyFrame Multi-Thread Server 测试 ==="

# 1. 基础功能测试
echo "1. 测试基础接口..."
curl -s $SERVER_URL/api/thread-info | jq -r '.current_thread' > /dev/null
if [ $? -eq 0 ]; then
    echo "✓ 线程信息接口正常"
else
    echo "✗ 线程信息接口异常"
    exit 1
fi

# 2. 重置统计
echo "2. 重置统计信息..."
curl -s -X POST $SERVER_URL/api/reset-stats > /dev/null

# 3. 并发测试
echo "3. 执行并发测试 (50个请求)..."
for i in {1..50}; do
    curl -s $SERVER_URL/api/load-test > /dev/null &
done
wait

# 4. 查看结果
echo "4. 测试结果:"
STATS=$(curl -s $SERVER_URL/api/thread-stats)
TOTAL_REQUESTS=$(echo $STATS | jq -r '.total_requests')
echo "   总请求数: $TOTAL_REQUESTS"

# 5. 验证负载均衡
THREAD_COUNT=$(echo $STATS | jq -r '.threads | length')
echo "   工作线程数: $THREAD_COUNT"

if [ $TOTAL_REQUESTS -eq 50 ] && [ $THREAD_COUNT -gt 1 ]; then
    echo "✓ 多线程负载均衡测试通过"
else
    echo "✗ 测试未通过"
fi

echo "完整统计信息:"
echo $STATS | jq .
```

### 运行测试脚本
```bash
chmod +x test_multi_thread_server.sh
./test_multi_thread_server.sh
```

## 错误测试

### 测试不存在的接口
```bash
# 404错误测试
curl http://127.0.0.1:9999/nonexistent

# 错误的HTTP方法
curl -X DELETE http://127.0.0.1:9999/api/thread-info
```

### 连接测试
```bash
# 测试连接超时
curl --connect-timeout 5 http://127.0.0.1:9999/api/thread-info

# 测试请求超时
curl --max-time 10 http://127.0.0.1:9999/api/load-test
```

## HTTP/2 测试 (如果支持)

### 强制使用HTTP/2
```bash
# 如果服务器支持HTTP/2
curl --http2 http://127.0.0.1:9999/api/thread-info

# 查看协议版本
curl -w "%{http_version}\n" -o /dev/null -s http://127.0.0.1:9999/
```

## 监控命令

### 实时监控请求
```bash
# 持续监控统计变化
watch -n 1 'curl -s http://127.0.0.1:9999/api/thread-stats | jq .'

# 简化的实时监控
while true; do
    STATS=$(curl -s http://127.0.0.1:9999/api/thread-stats)
    REQUESTS=$(echo $STATS | jq -r '.total_requests')
    echo "$(date): 总请求数 $REQUESTS"
    sleep 1
done
```

### 性能分析
```bash
# 测量响应时间
curl -w "@curl-format.txt" -o /dev/null -s http://127.0.0.1:9999/api/load-test

# curl-format.txt 内容:
#     time_namelookup:  %{time_namelookup}\n
#        time_connect:  %{time_connect}\n
#     time_appconnect:  %{time_appconnect}\n
#    time_pretransfer:  %{time_pretransfer}\n
#       time_redirect:  %{time_redirect}\n
#  time_starttransfer:  %{time_starttransfer}\n
#                     ----------\n
#          time_total:  %{time_total}\n
```

## 预期结果示例

### 正常响应示例
```json
// GET /api/thread-info
{
  "thread_count": 8,
  "current_thread": "140234567890",
  "request_id": 15
}

// GET /api/thread-stats  
{
  "total_requests": 156,
  "total_connections": 12,
  "total_messages": 8,
  "threads": [
    "Thread-140234567890: Req=52, Conn=4, Msg=2",
    "Thread-140234567891: Req=48, Conn=3, Msg=3",
    "Thread-140234567892: Req=56, Conn=5, Msg=3"
  ]
}
```

## 故障排除

### 连接拒绝
```bash
# 检查服务器是否启动
curl http://127.0.0.1:9999/
# 如果失败，检查：
# 1. 服务器是否正在运行
# 2. 端口是否正确
# 3. 防火墙设置
```

### 响应慢
```bash
# 检查服务器负载
curl -w "%{time_total}\n" -o /dev/null -s http://127.0.0.1:9999/api/load-test
```

### JSON解析错误
```bash
# 检查响应格式
curl -v http://127.0.0.1:9999/api/thread-stats
# 确保Content-Type: application/json
```