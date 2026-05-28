#!/usr/bin/env python3
"""Run the unified Quasar E2E torture test and print a JSON report."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[1]


def main() -> int:
    cmd = [
        sys.executable,
        "-m",
        "pytest",
        "quasar/tests/test_quasar_e2e_torture.py",
        "-m",
        "torture",
        "-q",
        "--tb=short",
    ]
    proc = subprocess.run(cmd, cwd=str(_REPO))
    return int(proc.returncode)


if __name__ == "__main__":
    raise SystemExit(main())
