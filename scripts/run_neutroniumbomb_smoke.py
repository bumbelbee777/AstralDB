#!/usr/bin/env python3
"""Quick smoke gate for neutroniumbomb at 100M rows/table."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
_SCRIPT = _REPO / "scripts" / "benchmark_neutroniumbomb_full.py"


def main() -> int:
    cmd = [
        sys.executable,
        str(_SCRIPT),
        "--rows",
        "100000000",
        "--queries",
        "10",
        "--timeout-sec",
        "3600",
    ]
    if len(sys.argv) > 1 and sys.argv[1] == "--astral" and len(sys.argv) > 2:
        cmd.extend(["--astral", sys.argv[2]])
    return subprocess.call(cmd, cwd=str(_REPO))


if __name__ == "__main__":
    raise SystemExit(main())
