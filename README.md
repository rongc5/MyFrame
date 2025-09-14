# MyFrame — Multi‑Protocol Networking Framework (HTTP/1.1, HTTPS, HTTP/2, WS/WSS)

MyFrame is a C++11 epoll‑based networking framework supporting HTTP/1.1, HTTPS (TLS), HTTP/2 (ALPN), and WebSocket (WS/WSS), with an event‑loop architecture and pluggable protocol detection.

## Quick Start

Build all (library + examples):

```
./scripts/build_cmake.sh all
```

Run example servers:

```
# HTTP (default 8080)
build/examples/http_server 8080 &

# HTTPS (default 7777) — provide certs via env or default paths
export MYFRAME_SSL_CERT=tests/common/cert/server.crt
export MYFRAME_SSL_KEY=tests/common/cert/server.key
build/examples/https_server 8443 &

# HTTP/2 (TLS + ALPN h2, default 7779)
build/examples/h2_server 7779 &
```

Router client (event‑loop; supports http/https/ws/wss/h2):

```
build/examples/router_client http://127.0.0.1:8080/hello --timeout-ms 5000
build/examples/router_client https://127.0.0.1:8443/hello --timeout-ms 5000
build/examples/router_client h2://127.0.0.1:7779/hello --timeout-ms 5000
```

Flags:
- `--timeout-ms N` — stop event loop after N ms if not completed
- `--no-accept-encoding` — disable default `Accept-Encoding: gzip`
- `--user-agent UA` — override default User‑Agent header

## Highlights

- HTTPS ALPN → HTTP/2 (fallback 1.1): hybrid HTTPS client with automatic mode select
- HTTP/2 client: preface/SETTINGS/HEADERS/DATA, PING timer + total timeout, WINDOW_UPDATE batching (32KB default)
- Correct `:scheme`, HPACK indexed names, `:authority` includes port
- Optional gzip auto‑decompression (HTTP/1.1 + H2) when zlib is available
- Default headers (when not provided):
  - HTTP/1.1: `User-Agent: myframe-http-client`, `Accept-Encoding: gzip`
  - HTTP/2: `user-agent: myframe-h2-client`, `accept-encoding: gzip`
  - Disable via `MYFRAME_NO_DEFAULT_AE=1` or router_client `--no-accept-encoding`
- TLS improvements: client double‑free fix, non‑blocking connect with SO_ERROR, session cache/tickets knobs

## Useful Env Knobs

- TLS server: `MYFRAME_SSL_CERT`, `MYFRAME_SSL_KEY`, `MYFRAME_SSL_PROTOCOLS`, `MYFRAME_SSL_ALPN`, `MYFRAME_SSL_SESS_CACHE(1/0)`, `MYFRAME_SSL_SESS_CACHE_SIZE`, `MYFRAME_SSL_TICKETS(1/0)`
- TLS client: set via code `tls_set_client_config(...)` or env `MYFRAME_SSL_VERIFY=1`, `MYFRAME_SSL_CA`, `MYFRAME_SSL_CLIENT_CERT`, `MYFRAME_SSL_CLIENT_KEY`
- H2 client: `MYFRAME_H2_PING_MS` (default 15000), `MYFRAME_H2_TIMEOUT_MS` (default 30000), `MYFRAME_H2_WINUPDATE` (bytes, default 32768), `MYFRAME_H2_TRACE=1` (frame logs)
- Listener backlog: `MYFRAME_SOMAXCONN` (default 1024)
- Disable default AE: `MYFRAME_NO_DEFAULT_AE=1`

## Performance Helpers

- HTTP bench: `scripts/perf/run_http_bench.sh <http|https> <port> [duration] [concurrency]`
- WS bench: `scripts/perf/run_ws_bench.sh <ws|wss> <host> <port> [path] [messages] [size]`

See also:
- docs/FRAMEWORK_FEATURES_CN.md / FRAMEWORK_FEATURES_EN.md
- docs/PERF_TESTING_CN.md
- docs/USAGE_GUIDE_CN.md (TLS/mTLS usage; default headers & gzip notes)
 - docs/CONNECTION_LIFECYCLE_CN.md (how to close connections safely)
