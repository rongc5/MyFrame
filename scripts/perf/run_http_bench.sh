#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/perf/run_http_bench.sh <http|https> <port> [duration=30s] [concurrency=256]
# Requires: wrk or hey (auto-detect). Outputs to out/perf/.

SCHEME=${1:-http}
PORT=${2:-8090}
DURATION=${3:-30s}
CONC=${4:-256}

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
OUT_DIR="$ROOT_DIR/out/perf/${SCHEME}_${PORT}_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

URL="${SCHEME}://127.0.0.1:${PORT}/hello"

echo "[perf] Target: $URL" | tee "$OUT_DIR/info.txt"
echo "[perf] Duration: $DURATION, Concurrency: $CONC" | tee -a "$OUT_DIR/info.txt"

run_wrk() {
  echo "[perf] Running wrk..." | tee -a "$OUT_DIR/info.txt"
  wrk -t4 -c "$CONC" -d "$DURATION" --latency "$URL" | tee "$OUT_DIR/wrk.txt"
}

run_hey() {
  echo "[perf] Running hey..." | tee -a "$OUT_DIR/info.txt"
  # hey duration like 30s: -z 30s ; no latency histogram but simple stats
  hey -z "$DURATION" -c "$CONC" "$URL" | tee "$OUT_DIR/hey.txt"
}

if command -v wrk >/dev/null 2>&1; then
  run_wrk
elif command -v hey >/dev/null 2>&1; then
  run_hey
else
  echo "[perf] Neither wrk nor hey found. Please install one of them." | tee -a "$OUT_DIR/info.txt"
  exit 2
fi

echo "[perf] Done. Reports under $OUT_DIR" | tee -a "$OUT_DIR/info.txt"

