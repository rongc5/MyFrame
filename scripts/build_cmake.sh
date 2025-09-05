#!/bin/bash

# CMake 构建辅助脚本（确保编译 core -> 目标 myframe）
# 用法：
#   ./scripts/build_cmake.sh                # 默认Debug，仅构建 myframe（core）
#   ./scripts/build_cmake.sh release        # Release 构建，仅构建 myframe
#   ./scripts/build_cmake.sh all            # 构建全部目标（examples/tests等）
#   ./scripts/build_cmake.sh clean          # 清理构建目录

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

cd "$PROJECT_ROOT"

echo "[MyFrame] Project root: $PROJECT_ROOT"

ACTION="${1:-}"
BUILD_TYPE="Debug"
BUILD_ALL=false

case "${ACTION}" in
  clean)
    echo "[MyFrame] Cleaning build directory $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    exit 0
    ;;
  release|Release|REL|rel)
    BUILD_TYPE="Release"
    ;;
  all|ALL)
    BUILD_ALL=true
    ;;
  "")
    # 默认参数，啥也不做
    ;;
  *)
    echo "[MyFrame] Unknown argument: ${ACTION}"
    echo "Usage: build_cmake.sh [clean|release|all]"
    exit 2
    ;;
esac

# 配置：显式使用 -S/-B，并设置构建类型
echo "[MyFrame] Configure (type=${BUILD_TYPE}) ..."
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

# 构建
if [[ "$BUILD_ALL" == true ]]; then
  echo "[MyFrame] Building ALL targets ..."
  cmake --build "$BUILD_DIR" -j"$(nproc)"
else
  echo "[MyFrame] Building target: myframe (core) ..."
  cmake --build "$BUILD_DIR" --target myframe -j"$(nproc)"
fi

echo "[MyFrame] Done. Artifacts:"
echo "  - Library: $BUILD_DIR/lib/libmyframe.a (由CMake拷贝)"
echo "  - Headers: $BUILD_DIR/include (由CMake同步)"
