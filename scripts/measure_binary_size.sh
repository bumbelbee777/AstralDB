#!/usr/bin/env bash
# Report raw and optional UPX-packed binary sizes.
set -euo pipefail

BIN="${1:?path to astraldb binary}"
UPX_BIN="${UPX_BIN:-${ASTRALDB_UPX_BIN:-upx}}"

raw=$(stat -c%s "$BIN")
echo "raw_bytes=$raw"

if command -v "$UPX_BIN" >/dev/null 2>&1; then
  tmp=$(mktemp)
  cp -f "$BIN" "$tmp"
  if "$UPX_BIN" --best --lzma --strip-relocs=0 --force-overwrite -q -o "$tmp" "$BIN" 2>/dev/null; then
    upx_bytes=$(stat -c%s "$tmp")
    echo "upx_bytes=$upx_bytes"
    awk -v r="$raw" -v u="$upx_bytes" 'BEGIN { printf "ratio=%.3f\n", u/r }'
  fi
  rm -f "$tmp"
fi
