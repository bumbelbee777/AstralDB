#!/usr/bin/env bash
set -euo pipefail

ROOT="${GITHUB_WORKSPACE:-$(cd "$(dirname "$0")/../.." && pwd)}"
ENT="${ROOT}/cmake/macos-jit.entitlements"
SIGN="${ROOT}/cmake/macos-sign-jit.sh"
RUN_TESTS="${ROOT}/build-ci/run_tests"
PROBE="${ROOT}/build-ci/jit_probe"
LOG="${ROOT}/build-ci/jit_probe.log"

fail() {
	echo "[macos-jit-probe] ERROR: $*" >&2
	exit 1
}

chmod +x "$SIGN"

echo "=== host ==="
uname -a
sw_vers 2>/dev/null || true
echo "shell=$BASH_VERSION"
echo "repo=$ROOT"

for BIN in "$RUN_TESTS" "$ENT"; do
	[[ -f "$BIN" ]] || fail "missing required file: $BIN"
done

echo "=== entitlements source ==="
cat "$ENT"

echo "=== run_tests pre-sign ==="
if [[ -f "$RUN_TESTS" ]]; then
	file "$RUN_TESTS" || true
	codesign -dv "$RUN_TESTS" 2>&1 | sed 's/^/  /' || true
else
	fail "run_tests not built yet: $RUN_TESTS"
fi

echo "=== sign run_tests ==="
bash "$SIGN" "$ENT" "$RUN_TESTS"

echo "=== build + sign jit_probe ==="
INVOKE_ASM="${ROOT}/sources/SQL/JIT/JitInvokeAarch64.S"
PROBE_CFLAGS=(-O0 -arch arm64 -Wall -Wextra)
if clang "${PROBE_CFLAGS[@]}" -mbranch-protection=none -c -o /dev/null "${ROOT}/tools/jit_aarch64_probe.c" 2>/dev/null; then
	PROBE_CFLAGS+=(-mbranch-protection=none)
fi
clang "${PROBE_CFLAGS[@]}" "${ROOT}/tools/jit_aarch64_probe.c" "$INVOKE_ASM" -o "$PROBE"
nm "$PROBE" 2>/dev/null | grep -q astraldb_jit_invoke_i64 || fail "trampoline symbol missing from jit_probe (link ${INVOKE_ASM})"
file "$PROBE" || true
bash "$SIGN" "$ENT" "$PROBE"

echo "=== execute jit_probe (log: $LOG) ==="
set +e
"$PROBE" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e

echo "=== probe exit code: $RC ==="
if [[ "$RC" -ne 0 ]]; then
	echo "[macos-jit-probe] FAILED — last 40 log lines:" >&2
	tail -n 40 "$LOG" >&2 || true
	echo "[macos-jit-probe] hints:" >&2
	echo "  - exit 139 in parent: invoke in fork child; sample must live in JIT page on VMAPPLE" >&2
	echo "  - rc=13 + signal 11: execute fault (signing/entitlements/W^X)" >&2
	echo "  - rc=12 on ret-smoke: execute OK but wrong exit check (fixed in probe)" >&2
	echo "  - rc=12 on sum test: ran but wrong sum (bytecode bug)" >&2
	echo "  - supported_np=0: use anon+mprotect first (MAP_JIT stays RW-only on VMAPPLE)" >&2
	echo "  - rc=1..4: publish failed (see errno in log)" >&2
	echo "  - hardened_runtime=1 in log: codesign still has --options runtime" >&2
	echo "  - export ASTRALDB_JIT_TRACE=1 and re-run ctest for in-app publish traces" >&2
	fail "jit_probe exited $RC"
fi

echo "=== jit_probe ok ==="
grep -E '^ok strategy=' "$LOG" || fail "probe succeeded but no ok line in log"
