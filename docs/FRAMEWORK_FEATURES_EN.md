# MyFrame Capabilities and Recent Changes

This document summarizes the currently supported features of the framework and the key improvements delivered in the latest changes, along with usage examples.

## Capabilities
- Protocols and features
  - HTTP/1.1 server and client (GET/POST, Content-Length/Chunked).
  - WebSocket server and client (WS/WSS) with application callbacks.
  - HTTPS/WSS (TLS 1.2/1.3, SNI; certificate verification can be added as needed).
  - HTTP/2:
    - Server: TLS + ALPN "h2" (see `examples/simple_h2_server.cpp`).
    - Client: event-loop client via `h2://` scheme; synchronous demo in `examples/simple_h2_client.cpp`.
- Event loop and threading
  - `base_net_thread` + `common_obj_container` + epoll-driven model.
  - Client connections can be registered into the container and are fully driven by the event loop.
  - Listener thread and worker pool with round-robin dispatch.
- Unified routing
  - `ClientConnRouter` chooses a builder by URL scheme: `http/https/ws/wss/h2`.
  - Simplifies client usage with the event loop.

## New in this update
1) HTTPS auto-upgrade to HTTP/2 via ALPN (fallback to HTTP/1.1)
- Added `core/hybrid_https_client_process.{h,cpp}`:
  - After TLS handshake, read ALPN result.
  - If `h2` is selected, delegate to `http2_client_process` to run HTTP/2.
  - Otherwise, perform a minimal one-shot HTTP/1.1 request/response (supports Content-Length and chunked), then stop the event threads.
- `client_conn_router2.cpp`: `https` builder now uses `tls_out_connect<hybrid_https_client_process>` with ALPN list `"h2,http/1.1"`.
- `client_ssl_codec.h`: added `selected_alpn()` to expose negotiated ALPN.

2) HTTP/2 client enhancements
- In `http2_client_process`:
  - PING timer (15s); replies with ACK to peer PING.
  - Total timeout (30s); stops threads if not completed.
  - Flow control optimization: send WINDOW_UPDATE in 32KB batches.

3) Connection and event robustness
- `out_connect::connect()` now uses `getaddrinfo` (hostname + IPv6 support), non-blocking connect, tries multiple addresses on failure.
- Monitor `EPOLLRDHUP` by default and treat as error in `event_process()` to quickly reclaim half-closed sockets.
- Fixed several PDEBUG format specifiers (size_t/ssize_t, pointers).

## Usage
- Build: `./scripts/build_cmake.sh all`
- Event-loop examples
  - HTTP/1.1: `build/examples/router_client http://127.0.0.1:7782/hello`
  - HTTPS (prefer HTTP/2, fallback to 1.1): `build/examples/router_client https://127.0.0.1:7779/hello`
  - Force HTTP/2: `build/examples/router_client h2://127.0.0.1:7779/hello`
- Business client (headers/method/body, WS echo):
  - `build/examples/router_biz_client http://127.0.0.1:7782/echo --method POST --H 'Content-Type: application/json' --data '{"x":1}'`

## Event-loop integration
- Preferred: `ClientConnRouter::create_and_enqueue(url, ctx)`
  - The builder installs the connection into the container and calls `connect()`.
  - `common_obj_container::obj_process()` drives `real_net_process()` and `epoll_wait()`.
- Manual (HTTP/1.1):
  - Create `out_connect<http_req_process>`, set process, call `set_net_container(container)`, then `connect()`.

## Notes and future work
- Possible improvements:
  - Switch to epoll edge-triggered mode (ET) and batch/zero-copy I/O (requires buffer and loop refactor).
  - Full HPACK/Huffman and header mapping improvements (client/server).
  - Optionally reuse `http_req_process/http_client_data_process` for HTTPS→HTTP/1.1 fallback (current minimal implementation is lightweight and robust).

## Default Headers and gzip
- HTTP/1.1 client (auto-injected if not provided):
  - `User-Agent: myframe-http-client`
  - `Accept-Encoding: gzip`
  - Also fills `Host` and `Connection: close` when missing.
- HTTP/2 client (auto-injected if not provided):
  - `user-agent: myframe-h2-client`
  - `accept-encoding: gzip`
- Auto-decompression:
  - If zlib is available at build time, gzip responses are auto-decompressed (both HTTP/1.1 and H2) with decoded length and small-body preview.
  - Without zlib, behavior falls back to raw output.
- Disable default Accept-Encoding (optional):
  - Set env `MYFRAME_NO_DEFAULT_AE=1` to prevent auto-injecting `Accept-Encoding: gzip` for HTTP/1.1 and HTTP/2 clients.
  - `router_client` supports `--no-accept-encoding` which sets the env internally.
