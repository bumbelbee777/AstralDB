#!/usr/bin/env bash
# Local helper: CMake Release build + full ctest (CI uses `ctest -L fast`; see docs/RELEASING.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-ci}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
GENERATOR="${GENERATOR:-Ninja}"
CXX="${CXX:-g++}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found" >&2
  exit 1
fi

JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -G "$GENERATOR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_CXX_COMPILER="$CXX"

cmake --build "$BUILD_DIR" -j "$JOBS"

cd "$ROOT"
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "OK: tests passed (build: $BUILD_DIR)"
