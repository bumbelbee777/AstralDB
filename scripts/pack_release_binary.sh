#!/usr/bin/env bash
# Pack a Release binary with UPX (Linux ELF). Keeps .unpacked backup beside input.
set -euo pipefail

INPUT="${1:?path to binary}"
UPX_BIN="${UPX_BIN:-${ASTRALDB_UPX_BIN:-upx}}"

if ! command -v "$UPX_BIN" >/dev/null 2>&1; then
  echo "UPX not found: $UPX_BIN" >&2
  exit 1
fi

cp -f "$INPUT" "${INPUT}.unpacked"
"$UPX_BIN" --best --lzma --strip-relocs=0 --force-overwrite -q -o "$INPUT" "${INPUT}.unpacked"
"$UPX_BIN" -t "$INPUT"
echo "Packed $(basename "$INPUT"): $(stat -c%s "${INPUT}.unpacked") -> $(stat -c%s "$INPUT") bytes"
