#!/usr/bin/env bash
# Ad-hoc sign a Mach-O executable for MAP_JIT (allow-jit + hardened runtime + DER).
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: $0 ENTITLEMENTS.plist EXECUTABLE" >&2
	exit 2
fi

ENT="$1"
EXE="$2"

if [[ ! -f "$ENT" ]]; then
	echo "entitlements missing: $ENT" >&2
	exit 1
fi
if [[ ! -f "$EXE" ]]; then
	echo "executable missing: $EXE" >&2
	exit 1
fi

codesign -s - --entitlements "$ENT" --options runtime \
	--generate-entitlement-der --timestamp=none --force "$EXE"
codesign --verify --strict "$EXE"

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
codesign -d --entitlements "$TMP" "$EXE" 2>/dev/null || true
if [[ ! -s "$TMP" ]] || ! grep -qE 'com.apple.security.cs.allow-(jit|unsigned-executable-memory)' "$TMP"; then
	echo "JIT entitlement not present after signing $EXE" >&2
	echo "extracted entitlements:" >&2
	cat "$TMP" >&2 || true
	exit 1
fi
