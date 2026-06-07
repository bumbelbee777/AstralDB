#!/usr/bin/env python3
"""ACID interrupt/resume demo: checkpoint mid-megafusion, resume, finish sub-1s wall."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
if str(_REPO / "scripts") not in sys.path:
    sys.path.insert(0, str(_REPO / "scripts"))

from bench_timing_util import parse_time_sql_output, resolve_astral_executable  # noqa: E402

BULK_RX = re.compile(r"BULK\s+\d+", re.IGNORECASE)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=5_000)
    ap.add_argument("--opt", default="-O4")
    args = ap.parse_args()

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable.", file=sys.stderr)
        return 2

    tmpdir = Path(tempfile.mkdtemp(prefix="nuke_acid_"))
    db_path = tmpdir / "astraldb_session_acid_demo.db"
    ckpt_path = Path(str(db_path) + ".query.ckpt")

    setup = BULK_RX.sub(
        f"BULK {args.rows}",
        (_REPO / "examples/benchmarks/neutroniumbomb_demo_setup.sql").read_text(encoding="utf-8"),
    )
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup, encoding="utf-8")
    query_path = _REPO / "examples/benchmarks/neutroniumbomb_query.sql"

    env = os.environ.copy()
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(args.rows, 10_000))
    env["ASTRALDB_DISABLE_BULK_SPILL"] = "1"
    env.setdefault("ASTRALDB_WAL_FSYNC_ASYNC", "1")
    env["ASTRALDB_MEGAFUSION_DEMO_CKPT"] = "1"

    run1_cmd = [
        str(astral),
        args.opt,
        "--database",
        str(db_path),
        "--time-sql-setup",
        str(setup_path),
        "--time-sql",
        str(query_path),
        "--time-sql-durable",
    ]
    proc1 = subprocess.run(run1_cmd, capture_output=True, text=True, env=env, cwd=str(_REPO))
    if proc1.returncode != 0:
        print(proc1.stderr or proc1.stdout, file=sys.stderr)
        return proc1.returncode
    if not ckpt_path.is_file():
        print(proc1.stderr or proc1.stdout, file=sys.stderr)
        print("Expected query checkpoint file after phase-1 run.", file=sys.stderr)
        return 3

    env.pop("ASTRALDB_MEGAFUSION_DEMO_CKPT", None)
    run2_cmd = [
        str(astral),
        args.opt,
        "--database",
        str(db_path),
        "--time-sql",
        str(query_path),
        "--resume-checkpoint",
        str(ckpt_path),
        "--time-sql-durable",
    ]
    t0 = time.perf_counter()
    proc2 = subprocess.run(run2_cmd, capture_output=True, text=True, env=env, cwd=str(_REPO))
    wall_ms = (time.perf_counter() - t0) * 1000.0
    timing = parse_time_sql_output(proc2.stdout + "\n" + proc2.stderr)
    print(f"resume execute_ms={timing.execute_ms}  total_wall_ms={wall_ms:.1f}")
    print(
        f"scanned_rows={timing.scanned_rows}  result_rows={timing.result_rows}  "
        f"wal_quiesce_ms={timing.wal_quiesce_ms}"
    )
    if proc2.returncode != 0:
        print(proc2.stderr, file=sys.stderr)
        return proc2.returncode
    if wall_ms > 1000.0:
        print(f"wall budget exceeded: {wall_ms:.1f} ms", file=sys.stderr)
        return 4
    min_scan = args.rows * 10
    if timing.scanned_rows is None or timing.scanned_rows < min_scan:
        print(f"resume scanned_rows below warehouse scale ({min_scan})", file=sys.stderr)
        return 5
    print("ACID interrupt/resume demo OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
