#!/usr/bin/env bash
set -euo pipefail

# One-click performance preset for myframe
# Usage:
#   source scripts/perf_preset.sh           # enable preset with defaults
#   source scripts/perf_preset.sh off       # disable preset and unset vars
#   source scripts/perf_preset.sh show      # print effective values

cmd=${1:-on}

print_effective() {
  echo "myframe effective perf env:" >&2
  printf '  %-24s = %s\n' \
    "MYFRAME_PERF_PRESET"  "${MYFRAME_PERF_PRESET-}"
  printf '  %-24s = %s\n' \
    "MYFRAME_EPOLL_WAIT_MS" "${MYFRAME_EPOLL_WAIT_MS-}"
  printf '  %-24s = %s\n' \
    "MYFRAME_IDLE_SKIP_MS"  "${MYFRAME_IDLE_SKIP_MS-}"
  printf '  %-24s = %s\n' \
    "MYFRAME_IDLE_SCAN_MAX" "${MYFRAME_IDLE_SCAN_MAX-}"
  printf '  %-24s = %s\n' \
    "MYFRAME_STRPOOL_CAP"   "${MYFRAME_STRPOOL_CAP-}"
  printf '  %-24s = %s\n' \
    "MYFRAME_THREAD_AFFINITY" "${MYFRAME_THREAD_AFFINITY-}"
  printf '  %-24s = %s\n' \
    "MYFRAME_EPOLL_SIZE"    "${MYFRAME_EPOLL_SIZE-}"
}

if [[ "$cmd" == "off" ]]; then
  unset MYFRAME_PERF_PRESET \
        MYFRAME_EPOLL_WAIT_MS \
        MYFRAME_IDLE_SKIP_MS \
        MYFRAME_IDLE_SCAN_MAX \
        MYFRAME_STRPOOL_CAP \
        MYFRAME_THREAD_AFFINITY \
        MYFRAME_EPOLL_SIZE || true
  echo "myframe perf preset disabled (env unset)." >&2
  return 0 2>/dev/null || exit 0
fi

if [[ "$cmd" == "show" ]]; then
  print_effective
  return 0 2>/dev/null || exit 0
fi

# Enable preset with conservative low-latency defaults.
# You can override any of these after sourcing this file.
export MYFRAME_PERF_PRESET=${MYFRAME_PERF_PRESET:-1}

# Optional explicit overrides (commented by default). Uncomment to pin.
# export MYFRAME_EPOLL_WAIT_MS=1     # default when preset=1 if unset
# export MYFRAME_IDLE_SKIP_MS=50     # default when preset=1 if unset
# export MYFRAME_IDLE_SCAN_MAX=0     # off by default
# export MYFRAME_STRPOOL_CAP=512     # default when preset=1 if unset
# export MYFRAME_THREAD_AFFINITY=1   # enabled when preset=1
# export MYFRAME_EPOLL_SIZE=1000     # default cap with clamp 256..65536

echo "myframe perf preset enabled." >&2
print_effective

