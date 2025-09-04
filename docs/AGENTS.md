# Repository Guidelines

## Project Structure & Module Organization
- `core/`: C++11 sources (networking, protocol detect, HTTP/WS, TLS, threading).
- `include/`: Public headers mirrored from `core/` and utils.
- `utils/`: Third‑party and helpers (`rapidjson/`, `str2sign/`).
- `lib/`: Built static libs (e.g., `libmyframe.a`).
- `scripts/`: Build helpers (`build.sh`, `build_cmake.sh`).
- `test/`: Unit/examples and simple clients/servers.
- `tests/`: Scenario/architecture cases (`case*/`), runner `run_all.sh` for CI.

## Build, Test, and Development Commands
- Fast build: `./scripts/build.sh` (or `./build.sh`).
- CMake build: `mkdir -p build && cd build && cmake .. && make -j$(nproc)`.
- Unit/examples: `cd test && make`; run: `./test_server_simple`, `./simple_http_client`.
- Scenario suite: `cd tests && bash run_all.sh` (generates dev TLS certs if missing).
- HTTPS/WSS quick check: start server (`./test_server_simple`), then `curl -k https://127.0.0.1:7777/hello`, `./simple_wss_client 7777`.

## Coding Style & Naming Conventions
- C++11, warnings on (`-Wall`), pthread, OpenSSL.
- Files: lower_snake_case; Types: PascalCase; funcs/vars: lower_snake_case; MACROS: UPPER_SNAKE_CASE.
- 4‑space indent, no tabs; same‑line braces; prefer `nullptr`, RAII. Use existing style; optionally run `clang-format` locally.

## Testing Guidelines
- Unit/examples in `test/` (prefix `test_...`); scenarios in `tests/case*/`.
- Cover protocol detection, HTTP/WS flows, TLS (HTTPS/WSS), and threading edges.
- Run locally before PR: `./scripts/build.sh && (cd tests && bash run_all.sh)`.

## Commit & Pull Request Guidelines
- Commits: Conventional Commits (`feat:`, `fix:`, `test:`, `build:`, `refactor:`).
- PRs include: scope/description, test plan (commands + output), linked issues, impact notes; attach logs/screenshots for behavior changes.
- CI must pass (`.github/workflows/test.yml` calls `tests/run_all.sh`).

## Security & Configuration Tips
- Dev certs live in `tests/common/cert/`. Do NOT commit real keys. Server cert override via `MYFRAME_SSL_CERT`/`MYFRAME_SSL_KEY`.
- Client TLS options: `MYFRAME_HTTP_SSL=1`, `MYFRAME_WS_SSL=1`, `MYFRAME_SSL_VERIFY=1`.
- Use non‑privileged ports locally; require OpenSSL dev, GCC/g++, and pthread.
