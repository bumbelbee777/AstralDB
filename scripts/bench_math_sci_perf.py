#!/usr/bin/env python3
"""Time MathSci DEQ_INTEGRATE + PREDICT vs legacy ODE_MARCH pattern."""

from __future__ import annotations

import argparse
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def run_sql(astral: Path, sql: str, db: Path) -> float:
    sql_path = db.parent / "bench.sql"
    sql_path.write_text(sql, encoding="utf-8")
    t0 = time.perf_counter()
    proc = subprocess.run(
        [str(astral), "--database", str(db), str(sql_path)],
        capture_output=True,
        text=True,
    )
    ms = (time.perf_counter() - t0) * 1000.0
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr or proc.stdout)
    return ms


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--runs", type=int, default=5)
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    astral = args.astral or repo / "build-release" / "Release" / "astraldb.exe"
    if not astral.is_file():
        astral = repo / "bin" / "astraldb.exe"
    if not astral.is_file():
        print("astraldb not found", file=sys.stderr)
        return 1

    dim = 32
    steps = 2000
    k = ",".join(["-0.01"] * dim)
    y = ",".join(["1.0"] + ["0.0"] * (dim - 1))
    deq_sql = f"""
DROP TABLE IF EXISTS t;
CREATE TABLE t (y TEXT);
INSERT INTO t VALUES (
  DEQ_INTEGRATE('RK4','L[{dim}]:{y}','0.001','{steps}',
    'L[{dim}]:{k}','L[{dim}]:{k}','L[{dim}]:{k}','L[{dim}]:{k}')
);
"""
    march_sql = f"""
DROP TABLE IF EXISTS t;
CREATE TABLE t (y TEXT);
INSERT INTO t VALUES ('L[{dim}]:{y}');
"""
    # Unrolled 50 ODE_MARCH-equivalent steps via DEQ 40 steps x 50 = 2000? Use single DEQ for fair compare
    legacy_sql = f"""
DROP TABLE IF EXISTS t;
CREATE TABLE t (y TEXT);
INSERT INTO t VALUES ('L[{dim}]:{y}');
"""
    # 50 SQL updates each calling SOLVE_ODE once = still slow; compare DEQ 2000 vs DEQ 100 x 20
    with tempfile.TemporaryDirectory() as tmp:
        tmp_p = Path(tmp)
        deq_times = []
        for _ in range(args.runs):
            db = tmp_p / "deq.db"
            if db.exists():
                db.unlink()
            deq_times.append(run_sql(astral, deq_sql, db))
        print(f"DEQ_INTEGRATE dim={dim} steps={steps}: median {statistics.median(deq_times):.2f} ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
