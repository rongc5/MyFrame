#!/bin/bash

# CMake 构建辅助脚本（确保编译 core -> 目标 myframe）
# 用法：
#   ./scripts/build_cmake.sh                # 默认Debug，仅构建 myframe（core）
#   ./scripts/build_cmake.sh release        # Release 构建，仅构建 myframe
#   ./scripts/build_cmake.sh all            # 构建全部目标（examples/tests等）
#   ./scripts/build_cmake.sh clean          # 清理构建目录

set -euo pipefail

# Auto-enable devtoolset-9 on CentOS/RHEL for C++17 support
if [[ -f /opt/rh/devtoolset-9/enable ]]; then
    set +u; source /opt/rh/devtoolset-9/enable; set -u
fi

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

# 配置：兼容 CMake 2.8+（不使用 -S/-B 参数）
echo "[MyFrame] Configure (type=${BUILD_TYPE}) ..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# ── OpenSSL selection ──────────────────────────────────────────────────────
# 优先级:
#   1) /usr/local/openssl/lib/libssl.a  + libcrypto.a   (静态, 强烈优先)
#   2) /opt/openssl11/lib/libssl.a       + libcrypto.a
#   3) /usr/local/openssl/lib/libssl.so + libcrypto.so  (动态, fallback)
#   4) /opt/openssl11/lib/libssl.so      + libcrypto.so
#   5) system default (cmake FindOpenSSL)
#
# 静态 (.a) 优先理由 (跟 sk_server 同思路): 二进制自包含, runtime 不再依赖
# /usr/lib64/libssl.so.X 当下指向哪个版本, 彻底消除 header/lib 跨版本错配
# (你 prod 上 1.0 headers + 3.0/1.1 libs 混着用的灾难). 唯一代价: binary
# 体积 +~10MB, 安全 patch 时要重链 (但反正每次升级都得重 build).
OPENSSL_CMAKE_ARGS=()
OPENSSL_DESC=""
for OSSL_ROOT in /usr/local/openssl /opt/openssl11; do
    if [[ ! -d "$OSSL_ROOT" ]]; then continue; fi
    if [[ -f "$OSSL_ROOT/lib/libssl.a" && -f "$OSSL_ROOT/lib/libcrypto.a" ]]; then
        OPENSSL_DESC="$OSSL_ROOT (static .a)"
        OPENSSL_CMAKE_ARGS=(
            "-DOPENSSL_ROOT_DIR=$OSSL_ROOT"
            "-DOPENSSL_INCLUDE_DIR=$OSSL_ROOT/include"
            "-DOPENSSL_SSL_LIBRARY=$OSSL_ROOT/lib/libssl.a"
            "-DOPENSSL_CRYPTO_LIBRARY=$OSSL_ROOT/lib/libcrypto.a"
            "-DOPENSSL_USE_STATIC_LIBS=TRUE"
        )
        break
    fi
    if [[ -f "$OSSL_ROOT/lib/libssl.so" && -f "$OSSL_ROOT/lib/libcrypto.so" ]]; then
        OPENSSL_DESC="$OSSL_ROOT (dynamic .so)"
        OPENSSL_CMAKE_ARGS=(
            "-DOPENSSL_ROOT_DIR=$OSSL_ROOT"
            "-DOPENSSL_INCLUDE_DIR=$OSSL_ROOT/include"
            "-DOPENSSL_SSL_LIBRARY=$OSSL_ROOT/lib/libssl.so"
            "-DOPENSSL_CRYPTO_LIBRARY=$OSSL_ROOT/lib/libcrypto.so"
        )
        break
    fi
done
[[ -z "$OPENSSL_DESC" ]] && OPENSSL_DESC="system default (cmake FindOpenSSL)"
echo "[MyFrame] OpenSSL: ${OPENSSL_DESC}"

# CMakeCache stickiness fix: 如果旧 CMakeCache.txt 里 OPENSSL_INCLUDE_DIR 跟
# 这次选择不一致, cmake 不会 override (cache 优先), 必须 wipe 重来。否则
# build 用旧 1.0 headers 但链 1.1 libs, 出 undefined reference 这种灾难。
if [[ -f "$BUILD_DIR/CMakeCache.txt" && ${#OPENSSL_CMAKE_ARGS[@]} -gt 0 ]]; then
    EXPECTED_INC=""
    for arg in "${OPENSSL_CMAKE_ARGS[@]}"; do
        case "$arg" in
            -DOPENSSL_INCLUDE_DIR=*) EXPECTED_INC="${arg#-DOPENSSL_INCLUDE_DIR=}" ;;
        esac
    done
    CACHED_INC=$(grep "^OPENSSL_INCLUDE_DIR" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
    if [[ -n "$EXPECTED_INC" && -n "$CACHED_INC" && "$EXPECTED_INC" != "$CACHED_INC" ]]; then
        echo "[MyFrame] Stale CMakeCache (cached=$CACHED_INC vs want=$EXPECTED_INC); wiping build/"
        cd "$PROJECT_ROOT"
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
    fi
fi

cmake "$PROJECT_ROOT" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" "${OPENSSL_CMAKE_ARGS[@]}"
cd "$PROJECT_ROOT"

# 构建
cd "$BUILD_DIR"
if [[ "$BUILD_ALL" == true ]]; then
  echo "[MyFrame] Building ALL targets ..."
  make -j"$(nproc)"
else
  echo "[MyFrame] Building target: myframe (core) ..."
  make myframe -j"$(nproc)"
fi
cd "$PROJECT_ROOT"

echo "[MyFrame] Done. Artifacts:"
echo "  - Library: $BUILD_DIR/lib/libmyframe.a (由CMake拷贝)"
echo "  - Headers: $BUILD_DIR/include (由CMake同步)"
