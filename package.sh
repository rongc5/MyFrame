#!/usr/bin/env bash
set -euo pipefail

# 兼容入口: 已统一到 scripts/pack_myframe.sh (读 VERSION, 归档名嵌入版本号+时间戳)。
# 本脚本仅转调, 参数原样透传。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/scripts/pack_myframe.sh" "$@"
