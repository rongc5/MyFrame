# 协议支持测试修正规划（HTTP / HTTPS / WS / WSS）

本计划用于系统性验证框架在单协议与多协议场景下，作为服务端与客户端的完整互通能力，并为 CI 场景提供可脚本化的落地方案。

## 目标与范围
- 协议覆盖：HTTP、HTTPS、WS、WSS。
- 运行模式：
  - 单协议端口（PlainOnly / TlsOnly）。
  - 同端口多协议（Auto：HTTP+HTTPS、WS+WSS）。
- 角色覆盖：服务端与客户端互通。 
- 可靠性：证书幂等生成、关键日志可观测、错误场景有明确失败信号。

## 前置准备
- 生成/复用测试证书（幂等）：
  - `bash tests/common/cert/make_cert.sh`
- 可选：指定证书（避免路径不一致）：
  - `export MYFRAME_SSL_CERT=tests/common/cert/server.crt`
  - `export MYFRAME_SSL_KEY=tests/common/cert/server.key`
- 构建：
  - `./scripts/build.sh && (cd test && make -f Makefile_standalone all)`
- 成功标准：首次 HTTPS/WSS 请求时，服务端打印：`[tls] Using cert='…' key='…'`。

## A. 单协议服务端验证（4 个用例）

**A1. HTTP-only（PlainOnly）**
- 启动：基于 `server + MultiProtocolFactory(PlainOnly)` 或 `simple_http_server` 监听 `127.0.0.1:7777`。
- 请求：`curl -v http://127.0.0.1:7777/hello`
- 断言：HTTP 200，响应包含 `Hello`；日志中无 `TLS handshake detected`。
- 负例：`curl -vk https://127.0.0.1:7777/hello` 握手失败/立即关闭。

**A2. HTTPS-only（TlsOnly）**
- 启动：`cd test && ./https_server_standalone 7778`（或任一 TlsOnly 示例）。
- 请求：`curl -vk https://127.0.0.1:7778/hello`
- 断言：HTTP 200；首次握手日志含 `[tls] Using cert=…`；`[detect] TLS connection - …`。
- 负例：`curl -v http://127.0.0.1:7778/hello` 失败/关闭。

**A3. WS-only（明文 WebSocket）**
- 启动：`cd test && ./simple_http_server 7781`（或 `server + PlainOnly` 支持 `/websocket`）。
- 握手：`curl -v -H 'Connection: Upgrade' -H 'Upgrade: websocket' http://127.0.0.1:7781/websocket`
- 客户端：`cd test && ./simple_ws_client 7781`
- 断言：`101 Switching Protocols`，echo/ping-pong 正常。

**A4. WSS-only（TLS WebSocket）**
- 启动：`cd test && ./wss_server_standalone 7782`
- 客户端：`cd test && ./test_wss_standalone 7782`
- 断言：`101 Switching Protocols`，后续帧收发正常；服务端日志含 `[tls] Using cert=…`。

## B. 客户端单独验证（4 个用例）

**B1. HTTP 客户端 → A1**
- 目标：A1 的 HTTP-only 服务器。
- 客户端：`cd test && ./test_http_client 7777` 或 `./simple_http_client 7777`。
- 断言：状态码 200，body/headers 符合预期。

**B2. HTTPS 客户端 → A2**
- 目标：A2 的 HTTPS-only 服务器。
- 客户端：`cd test && ./test_https_client_fw 7778`。
- 断言：状态码 200；若设置 `MYFRAME_SSL_VERIFY=1` 搭配 `MYFRAME_CA_FILE=tests/common/cert/ca.pem` 能通过校验。

**B3. WS 客户端 → A3**
- 目标：A3 的 WS-only 服务器。
- 客户端：`cd test && ./simple_ws_client 7781`。
- 断言：握手成功；`ping` → `pong`；echo 正常。

**B4. WSS 客户端 → A4**
- 目标：A4 的 WSS-only 服务器。
- 客户端：`cd test && ./test_wss_client_fw 7782`。
- 断言：握手成功；业务帧正常；服务端日志显示 TLS。

## C. 同端口多协议（Auto）

**C1. HTTP+HTTPS 同端口**
- 启动：`cd test && ./https_server_working 7790`（默认 `Auto` 模式）。
- 请求：
  - `curl -v http://127.0.0.1:7790/hello` → 200
  - `curl -vk https://127.0.0.1:7790/hello` → 200，首次握手触发 `[detect] TLS handshake detected!`
- 断言：两种协议同时可用；服务器日志展现 `TlsProbe → tls_entry_process → HttpProbe` 路径。

**C2. WS+WSS 同端口**
- 启动：自建 `server + MultiProtocolFactory(Auto)` 监听 `7791`，`on_ws` 回显。
- 请求：
  - 明文：`cd test && ./simple_ws_client 7791`
  - TLS：`cd test && ./test_wss_standalone 7791`
- 断言：两种升级路径均返回 `101`；后续帧正常；日志分别显示明文与 TLS 路径。

## D. 同端口注册两种协议（服务端）

**D1. HTTP+WS（同端口）**
- 启动：`Auto` 模式，`on_http` 与 `on_ws` 均实现，监听 `7792`。
- 请求：
  - `curl -v http://127.0.0.1:7792/hello` → 200
  - `curl -v -H 'Connection: Upgrade' -H 'Upgrade: websocket' http://127.0.0.1:7792/websocket` → 101
- 断言：同端口同时支持 HTTP 与 WS；`[detect]` 日志显示 `WsProbe` / `HttpProbe` 命中。

**D2. HTTPS+WSS（同端口）**
- 启动：`TlsOnly` 模式，`on_http` 与 `on_ws` 实现，监听 `7793`。
- 请求：
  - `curl -vk https://127.0.0.1:7793/hello` → 200
  - `cd test && ./test_wss_standalone 7793` → 101
- 断言：同端口在 TLS 上同时支持 HTTP 与 WS；`[tls]` → `protocol_detect_process(over_tls=true)` → `WsProbe/HttpProbe`。

## E. 负面与边界用例
- 明文流量打到 `TlsOnly` 端口：应拒绝或关闭；日志无 `HttpProbe` 处理。
- TLS 流量打到 `PlainOnly` 端口：`[detect] TLS handshake detected` 后无证书则失败并关闭；若有证书则不应接受（PlainOnly）。
- 非法路径：返回 404，且 `Connection: close` 行为一致。
- 并发稳健性：并行 50 次 `curl`（或 `xargs -P`）验证无崩溃、响应率 100%。
- 证书缺失：自动生成脚本被调用；失败则明确提示设置 `MYFRAME_SSL_CERT/KEY`。

## F. 自动化落地（建议）
- 目录结构：
  - `tests/case_http_plain/`
  - `tests/case_https_tls/`
  - `tests/case_ws_plain/`
  - `tests/case_wss_tls/`
  - `tests/case_dual_auto_http_https/`
  - `tests/case_dual_auto_ws_wss/`
  - `tests/case_dual_tlsonly_https_wss/`
- 每个 case：
  - `run.sh`：启动服务（后台/写 PID）、等待端口、执行请求、断言日志与响应、清理。
  - 统一断言工具（建议）：`assert_http 200`、`assert_contains "Using cert="` 等。
- 集成 `tests/run_all.sh`：顺序执行上述 case；开始时生成证书；`trap` 收尾。

## G. 关键日志断言点（建议 grep）
- TLS 初始化：`[tls] Using cert='…' key='…'`
- TLS 绑定 FD：`[tls] Binding SSL to fd=`
- 探测路径：
  - 明文→HTTP：`[HttpProbe] Match result: YES` → `http_res_process`
  - 明文→WS：请求含 `Upgrade: websocket` 命中 `WsProbe`
  - TLS 入站：`[detect] TLS handshake detected!` → `tls_entry_process`
  - TLS 上探测：`[detect] TLS connection - skipping MSG_PEEK`
- 监听：`[acceptor] ========== Bound to 127.0.0.1:PORT`
- 分发：`[acceptor] Dispatched fd=… to worker…`

## H. 常见问题（FAQ）
- curl 卡住/无响应：多因服务未监听、端口冲突、或证书路径未命中。先 `ss -ltnp | grep PORT`，并强制设定 `MYFRAME_SSL_CERT/KEY` 后重试。
- 期望日志未打印：注意 `[tls] Using cert=…` 在首次 TLS 握手时出现，而非启动时。
- 端口被占用：调整到 7777–7793 范围内的空闲端口；避免与其它示例冲突。

