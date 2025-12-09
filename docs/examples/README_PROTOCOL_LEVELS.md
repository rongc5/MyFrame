# MyFrame Protocol Level Examples

本页汇总了框架三层协议接口（Level 1/2/3）的示例，方便按需测试：

| Level | 示例可执行程序 | 协议 | 特性 | 启动命令示例 |
|-------|----------------|------|------|---------------|
| Level 1 (`IApplicationHandler`) | `level1_multi_protocol` | HTTP / HTTPS / WS / WSS / HTTP/2 | 同步业务、定时器/线程访问、`current_connection_id()` | `./build/examples/level1_multi_protocol --port 8080 --tls` |
| Level 2 (`IProtocolHandler`) | `level2_multi_protocol` | HTTP / HTTPS / WS / WSS / HTTP/2 / Binary (TLV) | 异步响应（`async_response`）、跨线程消息、定时器、`current_connection_id()` | `./build/examples/level2_multi_protocol --port 9090 --tls` |
| Level 3 (`base_data_process`) | `level3_custom_echo` | 自定义 RAW 文本协议 | 直接继承 `base_data_process`，完全自管理状态/发送 | `./build/examples/level3_custom_echo --port 7070` |

> 默认证书位于 `test_certs/server.crt` / `test_certs/server.key`，若需自定义，可通过 `--cert/--key` 覆盖。

## 验证建议

- **Level 1**
  ```bash
  curl -k https://127.0.0.1:8080/api/hello
  websocat wss://127.0.0.1:8080/ws
  nghttp -n 8080 https://127.0.0.1:8080/api/hello
  ```

- **Level 2**
  ```bash
  curl -k https://127.0.0.1:9090/async
  websocat wss://127.0.0.1:9090/ws
  printf 'ECHO\0\0\0\x05hello' | nc 127.0.0.1 9090
  ```

- **Level 3**
  ```bash
  printf 'RAW hello\n' | nc 127.0.0.1 7070
  printf 'RAW quit\n'  | nc 127.0.0.1 7070
  ```

更多关于三层接口的说明，请参考 `docs/UNIFIED_PROTOCOL_DESIGN.md`。
