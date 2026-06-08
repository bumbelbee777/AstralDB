#!/usr/bin/env bash
# Ad-hoc sign for MAP_JIT / mprotect JIT on macOS.
# Do NOT pass --options runtime: hardened runtime + ad-hoc "-" on CI does not
# grant executable JIT pages even when entitlements are embedded in the signature.
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

echo "[sign-jit] binary=$EXE"
echo "[sign-jit] entitlements=$ENT"
if command -v shasum >/dev/null 2>&1; then
	echo "[sign-jit] sha256=$(shasum -a 256 "$EXE" | awk '{print $1}')"
fi

echo "[sign-jit] before:"
codesign -dv "$EXE" 2>&1 | sed 's/^/[sign-jit]   /' || true

codesign -s - --entitlements "$ENT" --generate-entitlement-der --timestamp=none --force "$EXE"
codesign --verify --strict "$EXE"

echo "[sign-jit] after:"
codesign -dv "$EXE" 2>&1 | sed 's/^/[sign-jit]   /' || true

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
codesign -d --entitlements "$TMP" "$EXE" 2>/dev/null || true
echo "[sign-jit] embedded entitlements:"
sed 's/^/[sign-jit]   /' "$TMP" || true

if [[ ! -s "$TMP" ]] || ! grep -qE 'com.apple.security.cs.allow-(jit|unsigned-executable-memory)' "$TMP"; then
	echo "[sign-jit] ERROR: JIT entitlement not present after signing $EXE" >&2
	exit 1
fi

if codesign -dv "$EXE" 2>&1 | grep -qi 'runtime'; then
	echo "[sign-jit] WARNING: hardened runtime flag still set — JIT may SIGSEGV on CI" >&2
fi

echo "[sign-jit] ok $EXE"
