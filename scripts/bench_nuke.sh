#!/usr/bin/env bash
set -euo pipefail
CLI="${QUASAR_ASTRALDB:-./build-ci/astraldb}"
TIMEOUT_SEC="${NUKE_TIMEOUT_SEC:-180}"
MAX_VMEM_KB="${NUKE_MAX_VMEM_KB:-2097152}"
if [[ ! -x "$CLI" && -x "./build/astraldb" ]]; then
	CLI="./build/astraldb"
fi
if command -v timeout >/dev/null 2>&1; then
	exec timeout "${TIMEOUT_SEC}s" bash -lc "ulimit -v ${MAX_VMEM_KB}; \"$CLI\" -m -O4 --time-sql examples/nuke.sql"
fi
exec bash -lc "ulimit -v ${MAX_VMEM_KB}; \"$CLI\" -m -O4 --time-sql examples/nuke.sql"
