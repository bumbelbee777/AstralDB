#!/usr/bin/env bash
# Pack a Release binary with UPX (Linux ELF). Keeps .unpacked backup beside input.
set -euo pipefail

INPUT="${1:?path to binary}"
UPX_BIN="${UPX_BIN:-${ASTRALDB_UPX_BIN:-upx}}"

if ! command -v "$UPX_BIN" >/dev/null 2>&1; then
  echo "UPX not found: $UPX_BIN" >&2
  exit 1
fi

if [[ ! -f "${INPUT}.unpacked" ]]; then
  cp -f "$INPUT" "${INPUT}.unpacked"
fi
bash "$(dirname "$0")/upx_pack_copy.sh" "${INPUT}.unpacked" "$INPUT" >/dev/null
echo "Packed $(basename "$INPUT"): $(stat -c%s "${INPUT}.unpacked") -> $(stat -c%s "$INPUT") bytes"
