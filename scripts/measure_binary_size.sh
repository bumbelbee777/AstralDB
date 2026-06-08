#!/usr/bin/env bash
# Report raw and UPX-packed binary sizes (packs a copy; input may already be packed).
set -euo pipefail

BIN="${1:?path to astraldb binary}"
UPX_BIN="${UPX_BIN:-${ASTRALDB_UPX_BIN:-upx}}"

if [[ -f "${BIN}.unpacked" ]]; then
  SRC="${BIN}.unpacked"
else
  SRC="$BIN"
fi

raw=$(stat -c%s "$SRC")
echo "raw_bytes=$raw"

if ! command -v "$UPX_BIN" >/dev/null 2>&1; then
  echo "UPX not found: $UPX_BIN" >&2
  exit 1
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
if ! bash "$(dirname "$0")/upx_pack_copy.sh" "$SRC" "$tmp" >/dev/null; then
  echo "UPX pack failed for $SRC" >&2
  exit 1
fi
upx_bytes=$(stat -c%s "$tmp")
echo "upx_bytes=$upx_bytes"
awk -v r="$raw" -v u="$upx_bytes" 'BEGIN { printf "ratio=%.3f\n", u/r }'
