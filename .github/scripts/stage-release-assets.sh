#!/usr/bin/env bash
# Flatten CI artifacts into release/ and write SHA256SUMS (GNU coreutils format).
# Linux and Windows CLI binaries are UPX-packed when UPX is on PATH (see setup-upx.sh).
set -euo pipefail

artifacts_dir="${1:?artifacts directory}"
out_dir="${2:?output directory}"
require_upx="${REQUIRE_UPX:-1}"

mkdir -p "$out_dir"

require_file() {
  local path="$1"
  if [[ ! -f "$path" ]]; then
    echo "Missing artifact: $path" >&2
    echo "Contents of $artifacts_dir:" >&2
    find "$artifacts_dir" -maxdepth 3 -type f | sort >&2 || true
    exit 1
  fi
}

linux="${artifacts_dir}/astraldb-linux-amd64/astraldb"
macos="${artifacts_dir}/astraldb-macos-arm64/astraldb"
windows="${artifacts_dir}/astraldb-windows-amd64/astraldb.exe"

require_file "$linux"
require_file "$macos"
require_file "$windows"

install -m755 "$linux" "$out_dir/astraldb-linux-amd64"
install -m755 "$macos" "$out_dir/astraldb-macos-arm64"
cp "$windows" "$out_dir/astraldb-windows-amd64.exe"

UPX_BIN="${UPX_BIN:-${ASTRALDB_UPX_BIN:-upx}}"
if ! command -v "$UPX_BIN" >/dev/null 2>&1; then
  if [[ "$require_upx" == "1" ]]; then
    echo "UPX required for release assets but not found (set UPX or install via setup-upx.sh)" >&2
    exit 1
  fi
  echo "UPX not installed; shipping unpacked linux/windows binaries" >&2
else
  echo "Packing release CLIs with $UPX_BIN ($($UPX_BIN -V 2>/dev/null | head -1 || true))"
  repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
  bash "${repo_root}/scripts/pack_release_binary.sh" "$out_dir/astraldb-linux-amd64"
  bash "${repo_root}/scripts/pack_release_binary.sh" "$out_dir/astraldb-windows-amd64.exe"
  for f in "$out_dir/astraldb-linux-amd64" "$out_dir/astraldb-windows-amd64.exe"; do
    sz=$(stat -c%s "$f")
    echo "  $(basename "$f"): ${sz} bytes (packed)"
  done
fi

wheel="$(find "$artifacts_dir" -name 'quasar-*.whl' -print -quit)"
if [[ -z "$wheel" ]]; then
  echo "quasar wheel not found under $artifacts_dir" >&2
  find "$artifacts_dir" -maxdepth 3 -type f | sort >&2 || true
  exit 1
fi
cp "$wheel" "$out_dir/"

(
  cd "$out_dir"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -- * > SHA256SUMS
  else
    shasum -a 256 -- * | awk '{print $1 "  " $2}' > SHA256SUMS
  fi
)

echo "Release assets in $out_dir:"
ls -la "$out_dir"
echo "--- SHA256SUMS ---"
cat "$out_dir/SHA256SUMS"
