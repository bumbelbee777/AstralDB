#!/usr/bin/env bash
# Print GNU sha256sum-format line: "<hash>  <filename>"
set -euo pipefail

file="${1:?file path}"
base="$(basename "$file")"

if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$file" | awk -v f="$base" '{print $1 "  " f}'
elif command -v shasum >/dev/null 2>&1; then
  shasum -a 256 "$file" | awk -v f="$base" '{print $1 "  " f}'
else
  echo "sha256sum or shasum required" >&2
  exit 1
fi
