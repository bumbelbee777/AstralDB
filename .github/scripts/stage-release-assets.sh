#!/usr/bin/env bash
# Flatten CI artifacts into release/ and write SHA256SUMS (GNU coreutils format).
set -euo pipefail

artifacts_dir="${1:?artifacts directory}"
out_dir="${2:?output directory}"

mkdir -p "$out_dir"

install -m755 "$artifacts_dir/astraldb-linux-amd64/astraldb" "$out_dir/astraldb-linux-amd64"
install -m755 "$artifacts_dir/astraldb-macos-arm64/astraldb" "$out_dir/astraldb-macos-arm64"
cp "$artifacts_dir/astraldb-windows-amd64/astraldb.exe" "$out_dir/astraldb-windows-amd64.exe"

wheel="$(find "$artifacts_dir" -maxdepth 2 -name 'quasar-*.whl' -print -quit)"
if [[ -z "$wheel" ]]; then
  echo "quasar wheel not found under $artifacts_dir" >&2
  exit 1
fi
cp "$wheel" "$out_dir/"

(
  cd "$out_dir"
  sha256sum -- * > SHA256SUMS
)

echo "Release assets in $out_dir:"
ls -la "$out_dir"
echo "--- SHA256SUMS ---"
cat "$out_dir/SHA256SUMS"