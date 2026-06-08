#!/usr/bin/env bash
# Pack a copy of BIN with UPX; never mutate the input. Prints packed byte count on stdout.
set -euo pipefail

SRC="${1:?source binary}"
OUT="${2:?output path}"
UPX_BIN="${UPX_BIN:-${ASTRALDB_UPX_BIN:-upx}}"

if ! command -v "$UPX_BIN" >/dev/null 2>&1; then
  echo "UPX not found: $UPX_BIN" >&2
  exit 1
fi

cp -f "$SRC" "$OUT"
"$UPX_BIN" --best --lzma --strip-relocs=0 --force-overwrite -q -o "$OUT" "$SRC"
"$UPX_BIN" -t "$OUT" >/dev/null
stat -c%s "$OUT"
