# MyFrame 性能测试指南

本指南提供 HTTP/HTTPS/HTTP2/WS 的基准测试思路与可执行命令示例，帮助评估在不同并发与负载下的吞吐量（RPS）、延迟（p50/p90/p99）与资源占用。

## 环境准备
- 编译：`./scripts/build_cmake.sh all`
- 工具（任选其一或多种）：
  - `wrk`（推荐，支持自定义 Lua 脚本，带延迟分布）
  - `wrk2`（恒定速率负载）
  - `hey`（Go 实现，易安装）
  - `h2load`（nghttp2 提供，测 HTTP/2）
  - `ab`（ApacheBench，简单）
- 系统建议：
  - `ulimit -n 65535`
  - 可选：`sysctl -w net.core.somaxconn=4096 net.ipv4.ip_local_port_range="1024 65535" net.ipv4.tcp_tw_reuse=1`
  - 进程绑定 CPU：`taskset -c 0-3`（多核时）

## 服务端目标
- HTTP/1.1：`build_dbg/examples/http_server <port>`，默认端点 `/hello`、`/api/status`
- 多协议（HTTP+WS）：`build_dbg/examples/demo_multi_protocol_server <port>`
- HTTPS（ALPN h2/http1.1）：`build_dbg/examples/https_server <port>`
- HTTP/2（示例）：`build_dbg/examples/h2_server <port>`

## 基础场景

### 1) HTTP/1.1 吞吐
```bash
PORT=8090
./build_dbg/examples/http_server $PORT &
sleep 1

# wrk（首选）
wrk -t4 -c256 -d30s --latency http://127.0.0.1:$PORT/hello

# hey（备选）
hey -z 30s -c 256 http://127.0.0.1:$PORT/hello

# ab（备选）
ab -n 100000 -c 256 http://127.0.0.1:$PORT/hello
```

关注项：RPS、平均/中位/分位延迟、最大连接数的影响。逐步放大 `-c` 并记录转折点。

### 2) HTTPS（ALPN 自动升级）
```bash
PORT=8443
# 提供证书（或在代码中 tls_set_server_config）
export MYFRAME_SSL_CERT=tests/common/cert/server.crt
export MYFRAME_SSL_KEY=tests/common/cert/server.key
export MYFRAME_SSL_ALPN="h2,http/1.1"
./build_dbg/examples/https_server $PORT &
sleep 1

# wrk（需支持 OpenSSL）
wrk -t4 -c256 -d30s --latency https://127.0.0.1:$PORT/hello

# HTTP/2: h2load（如有）
h2load -n 100000 -c 200 -m 100 https://127.0.0.1:$PORT/hello
```

关注项：与 HTTP 相比的 RPS 衰减、握手影响（可预热）、开启/关闭证书校验的影响（客户端 `MYFRAME_SSL_VERIFY=1`）。

### 3) HTTP/2（明确 H2）
```bash
PORT=8443
# 如果 https_server 支持 ALPN h2，可直接用 h2load
h2load -n 100000 -c 200 -m 100 https://127.0.0.1:$PORT/hello

# 或强制 h2 客户端路径（事件循环客户端）
./build/examples/router_client h2://127.0.0.1:$PORT/hello
```

关注项：首包延迟、流控对长数据的影响（本框架已启用 32KB 批量 WINDOW_UPDATE）。

### 4) WebSocket 基础吞吐
- 建议以消息回显为基线场景，使用外部工具产生长连接并发送固定大小消息（如 1KB 文本）。
- 工具建议：自写小型压测器 or `websocat`/`autobahn-testsuite`。
- 路径：`demo_multi_protocol_server` 的 `/websocket`。

## 自动化脚本（可选）
项目内提供 `scripts/perf/run_http_bench.sh`，自动对 HTTP/1.1 与（若支持）HTTPS 进行基准并输出到 `out/perf/`。
```bash
./scripts/perf/run_http_bench.sh http 8090
./scripts/perf/run_http_bench.sh https 8443
```

## 指标采集建议
- 服务器侧：`pidstat -dur 1 -p <PID>`、`perf stat -p <PID>`、`top -H` 观察 CPU/上下文切换；`free -m` 观察内存；`ss -s` 观察 TCP 状态。
- 客户端侧：关注 RPS、延迟分布、失败率。

## 优化路线（结合结果）
- I/O 与事件：切换 EPOLL 边沿触发（ET），配合循环读写与缓冲池，减少 wakeup；批量 `writev`/聚合发送；限制发送队列以实现背压。
- 线程模型：根据 CPU 调整 worker 数；设置线程亲和性，减少迁移；监听与工作分离；必要时多监听 FD 分摊。
- TCP/TLS：调优 `somaxconn`、`tcp_tw_reuse`、端口范围；开启 TLS 会话缓存/票据以降低握手成本。
- HTTP/2：窗口更新阈值与帧批量；PING 周期与超时策略；合理设置 `MAX_CONCURRENT_STREAMS`（服务端）。

