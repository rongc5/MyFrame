#!/bin/bash

# Modern CMake build helper for MyFrame
# Usage: ./scripts/build_cmake.sh [clean]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

cd "$PROJECT_ROOT"

echo "[MyFrame] Project root: $PROJECT_ROOT"

if [[ "$1" == "clean" ]]; then
    echo "[MyFrame] Cleaning build directory $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    exit 0
fi

# Configure & build with CMake
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[MyFrame] Running CMake configure ..."
cmake ..

echo "[MyFrame] Building ..."
make -j"$(nproc)"

echo "[MyFrame] Build completed. Binaries are in $BUILD_DIR"