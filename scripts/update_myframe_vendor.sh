#!/usr/bin/env bash
set -euo pipefail

# Sync a local MyFrame build into this repo's vendored 3rdpartyinc/ and 3rdpartylib/
# Usage:
#   scripts/update_myframe_vendor.sh [REL_PATH_TO_MYFRAME]
#
# Notes:
# - Uses ONLY relative paths (no hard-coded absolutes).
# - Defaults to a nearby ../../ or ../../../ myframe relative to this script.
# - Builds MyFrame via its scripts/build_cmake.sh if present; falls back to CMake directly.

SCRIPT_DIR=$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

# Candidate relative locations for the myframe repo (from this script dir)
ARG_REL="${1:-}"
candidates=()
if [[ -n "$ARG_REL" ]]; then candidates+=("$ARG_REL"); fi
candidates+=("../../../myframe" "../../myframe" "../myframe")

MYFRAME_DIR=""
for rel in "${candidates[@]}"; do
  try_dir=$(cd -- "$SCRIPT_DIR/$rel" 2>/dev/null && pwd || true)
  if [[ -n "$try_dir" && -d "$try_dir" ]]; then MYFRAME_DIR="$try_dir"; break; fi
done

if [[ -z "$MYFRAME_DIR" ]]; then
  echo "[ERROR] MyFrame path not found. Tried: ${candidates[*]} (relative to $SCRIPT_DIR)" >&2
  echo "        Pass an explicit relative path: scripts/update_myframe_vendor.sh ../../some/path/to/myframe" >&2
  exit 1
fi

echo "[INFO] Using MyFrame at: $MYFRAME_DIR"

# Build MyFrame (library + exported headers under build/include)
if [[ -x "$MYFRAME_DIR/scripts/build_cmake.sh" ]]; then
  echo "[INFO] Building MyFrame via scripts/build_cmake.sh (release)"
  (cd "$MYFRAME_DIR" && bash ./scripts/build_cmake.sh release)
else
  echo "[INFO] Building MyFrame via CMake (Release)"
  (cd "$MYFRAME_DIR" && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release)
  (cd "$MYFRAME_DIR" && cmake --build build --target myframe -j"$(nproc 2>/dev/null || echo 4)")
fi

LIB_SRC="$MYFRAME_DIR/build/lib/libmyframe.a"
INC_SRC="$MYFRAME_DIR/build/include"
if [[ ! -f "$LIB_SRC" ]]; then
  echo "[ERROR] Built library not found: $LIB_SRC" >&2
  exit 1
fi
if [[ ! -d "$INC_SRC" ]]; then
  echo "[ERROR] Exported headers dir not found: $INC_SRC" >&2
  exit 1
fi

LIB_DST="$REPO_ROOT/3rdpartylib/libmyframe.a"
INC_DST="$REPO_ROOT/3rdpartyinc/myframe"

echo "[INFO] Syncing headers -> $INC_DST"
mkdir -p "$INC_DST"
rsync -a --delete "$INC_SRC/" "$INC_DST/"

echo "[INFO] Copying static library -> $LIB_DST"
mkdir -p "$(dirname "$LIB_DST")"
cp -f "$LIB_SRC" "$LIB_DST"

echo "[OK] MyFrame synced to vendor (headers+lib)."
echo "     - Include dir: 3rdpartyinc/myframe"
echo "     - Library:     3rdpartylib/libmyframe.a"

