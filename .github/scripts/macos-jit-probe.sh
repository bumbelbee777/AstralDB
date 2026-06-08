#!/usr/bin/env bash
set -euo pipefail

ROOT="${GITHUB_WORKSPACE:-$(cd "$(dirname "$0")/../.." && pwd)}"
ENT="${ROOT}/cmake/macos-jit.entitlements"
SIGN="${ROOT}/cmake/macos-sign-jit.sh"
RUN_TESTS="${ROOT}/build-ci/run_tests"
PROBE="${ROOT}/build-ci/jit_probe"

chmod +x "$SIGN"

echo "=== sign run_tests ==="
bash "$SIGN" "$ENT" "$RUN_TESTS"

echo "=== build + sign jit_probe ==="
clang -O0 "${ROOT}/tools/jit_aarch64_probe.c" -o "$PROBE"
bash "$SIGN" "$ENT" "$PROBE"

echo "=== execute jit_probe ==="
"$PROBE"
