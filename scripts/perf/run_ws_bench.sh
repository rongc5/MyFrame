#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/perf/run_ws_bench.sh <ws|wss> <host> <port> [path=/websocket] [messages=10000] [size=32]

SCHEME=${1:-ws}
HOST=${2:-127.0.0.1}
PORT=${3:-8090}
PATH=${4:-/websocket}
MSGS=${5:-10000}
SIZE=${6:-32}

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
URL="${SCHEME}://${HOST}:${PORT}${PATH}"
OUT_DIR="$ROOT_DIR/out/perf/ws_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

echo "[ws-bench] Target: $URL, messages=$MSGS, size=$SIZE" | tee "$OUT_DIR/info.txt"

if [ ! -x "$ROOT_DIR/build_dbg/examples/ws_bench_client" ]; then
  echo "[ws-bench] ws_bench_client not found; build examples first." | tee -a "$OUT_DIR/info.txt"
  exit 2
fi

"$ROOT_DIR/build_dbg/examples/ws_bench_client" "$URL" --messages "$MSGS" --size "$SIZE" --timeout-ms 60000 | tee "$OUT_DIR/bench.txt"

echo "[ws-bench] Done. Reports under $OUT_DIR" | tee -a "$OUT_DIR/info.txt"

