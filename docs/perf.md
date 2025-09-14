Performance Preset (One-Click)

Quick start
- Enable preset for current shell: `source scripts/perf_preset.sh`
- Disable and unset: `source scripts/perf_preset.sh off`
- Show effective env: `source scripts/perf_preset.sh show`

What it does
- Sets `MYFRAME_PERF_PRESET=1`. Code already uses this to pick tuned defaults:
  - Epoll wait: 1ms if `MYFRAME_EPOLL_WAIT_MS` unset.
  - Idle skip: 50ms if `MYFRAME_IDLE_SKIP_MS` unset.
  - String pool cap: 512 if `MYFRAME_STRPOOL_CAP` unset.
  - Thread affinity: enabled unless `MYFRAME_THREAD_AFFINITY=0`.

Optional overrides (examples)
- `export MYFRAME_EPOLL_WAIT_MS=2`
- `export MYFRAME_IDLE_SKIP_MS=100`
- `export MYFRAME_IDLE_SCAN_MAX=200`
- `export MYFRAME_STRPOOL_CAP=1024`
- `export MYFRAME_THREAD_AFFINITY=0`
- `export MYFRAME_EPOLL_SIZE=4096`

Notes
- Ranges and clamps exist in code to keep values reasonable.
- Place overrides after sourcing the script to ensure they stick.

