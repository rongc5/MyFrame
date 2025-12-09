# MyFrame

C++11 epoll-based networking framework with automatic protocol detection for HTTP/1.1, HTTPS/TLS, HTTP/2 (ALPN), and WebSocket.

## Build

```bash
./scripts/build_cmake.sh all
```

Alternative:

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

## Run examples

HTTP server (defaults to 8080):

```bash
build/examples/http_server 8080
```

HTTPS server (defaults to 8443):

```bash
MYFRAME_SSL_CERT=test_certs/server.crt \
MYFRAME_SSL_KEY=test_certs/server.key \
build/examples/https_server 8443
```

HTTP/2 server over TLS (defaults to 8443 with ALPN h2):

```bash
build/examples/h2_server 8443
```

Multi-protocol router client (HTTP/HTTPS/WS/WSS/H2):

```bash
build/examples/router_client http://127.0.0.1:8080/hello --timeout-ms 5000
build/examples/router_client https://127.0.0.1:8443/hello --timeout-ms 5000
build/examples/router_client h2://127.0.0.1:8443/hello --timeout-ms 5000
```

## Testing

```bash
cd test
make
```

## Documentation

Key references live in `docs/` (design, async response guide, timer quick reference, TLS setup, and feature lists).
