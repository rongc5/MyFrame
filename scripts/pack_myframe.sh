#!/usr/bin/env bash
set -euo pipefail

# myframe 源码打包脚本。
# 放在 myframe/scripts 下, 任意目录均可执行。
# 归档写到 myframe 的同级目录, 包内顶层目录保持为 myframe。
# 归档名嵌入 VERSION 里的版本号与打包时间戳; 编译产物/临时文件/已有归档不打进去。
#
# 版本号取自 myframe/VERSION 第一行的最后一个字段 (形如 `myframe 1.0.0` -> 1.0.0)。
#
# 用法:
#   scripts/pack_myframe.sh                 # -> ../myframe_v<VER>_full_<STAMP>.tar.gz
#   scripts/pack_myframe.sh myname          # -> ../myname_<STAMP>.tar.gz
#   scripts/pack_myframe.sh /abs/path.tar.gz

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BASE_DIR="$(basename "$ROOT_DIR")"
PARENT_DIR="$(dirname "$ROOT_DIR")"
STAMP="$(date +%Y%m%d_%H%M%S)"

# 读取版本号 (VERSION 第一行最后一个字段); 缺失则 unknown。
VER="$(awk 'NR==1{print $NF}' "$ROOT_DIR/VERSION" 2>/dev/null || true)"
VER="${VER:-unknown}"

OUT_PREFIX="${1:-${BASE_DIR}_v${VER}_full}"
OUT_PREFIX="${OUT_PREFIX%.tar.gz}"
if [[ "$OUT_PREFIX" = /* ]]; then
  OUT_PATH="${OUT_PREFIX}_${STAMP}.tar.gz"
else
  OUT_PATH="$PARENT_DIR/${OUT_PREFIX}_${STAMP}.tar.gz"
fi

echo "project : $BASE_DIR"
echo "version : $VER"
echo "packing : $ROOT_DIR"
echo "output  : $OUT_PATH"

tar -C "$PARENT_DIR" -czf "$OUT_PATH" \
  --exclude="$BASE_DIR/.git" \
  --exclude="$BASE_DIR/.claude" --exclude="$BASE_DIR/**/.claude" \
  --exclude="$BASE_DIR/.tmp" \
  --exclude="$BASE_DIR/**/.tmp" \
  --exclude="$BASE_DIR/build" \
  --exclude="$BASE_DIR/build_test" \
  --exclude="$BASE_DIR/build_fix" \
  --exclude="$BASE_DIR/out" \
  --exclude="$BASE_DIR/**/CMakeFiles" \
  --exclude="$BASE_DIR/**/CMakeCache.txt" \
  --exclude="$BASE_DIR/**/cmake_install.cmake" \
  --exclude="$BASE_DIR/**/Makefile" \
  --exclude="$BASE_DIR/*.tar.gz" \
  --exclude="$BASE_DIR/**/*.tar.gz" \
  --exclude="$BASE_DIR/**/*.o" \
  --exclude="$BASE_DIR/**/*.a" \
  --exclude="$BASE_DIR/**/*.so" \
  --exclude="$BASE_DIR/**/*.so.*" \
  --exclude="$BASE_DIR/**/*.log" \
  --exclude="$BASE_DIR/**/core" \
  --exclude="$BASE_DIR/**/*.core" \
  --exclude="$BASE_DIR/**/log" \
  --exclude="$BASE_DIR/**/logs" \
  --exclude="$BASE_DIR/**/crash" \
  "$BASE_DIR"

echo "done    : $OUT_PATH"
echo "size    : $(du -h "$OUT_PATH" | cut -f1)"
