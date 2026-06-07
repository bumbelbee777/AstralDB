#!/usr/bin/env bash
# Quick bloat audit helpers (requires bloaty/llvm-nm on PATH).
set -euo pipefail

BIN="${1:-build-ci/astraldb}"

if [[ ! -f "$BIN" ]]; then
  echo "Missing binary: $BIN" >&2
  exit 1
fi

echo "=== sections ==="
if command -v bloaty >/dev/null 2>&1; then
  bloaty "$BIN" -d compileunits | head -40
  echo "--- symbols ---"
  bloaty "$BIN" -d symbols | head -30
elif command -v llvm-nm >/dev/null 2>&1; then
  llvm-nm --print-size --size-sort "$BIN" | tail -40
else
  ls -la "$BIN"
fi
