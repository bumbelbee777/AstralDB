#!/usr/bin/env python3
"""Plot neutroniumbomb setup + Q10 timings and derived rows/sec."""

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
DEFAULT_ROWS = 100_000


def bench_env(rows: int) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "1")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows, 10_000))
    env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    return env


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=DEFAULT_ROWS)
    ap.add_argument("--output", type=Path, default=_REPO / "media" / "neutroniumbomb_bench.png")
    ap.add_argument("--runs", type=int, default=3)
    args = ap.parse_args()

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("pip install matplotlib", file=sys.stderr)
        return 2

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable.", file=sys.stderr)
        return 2

    setup_src = BULK_RX.sub(f"BULK {args.rows}", (_REPO / "examples/benchmarks/neutroniumbomb_setup.sql").read_text())
    tmpdir = Path(tempfile.mkdtemp(prefix="nuke_plot_"))
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup_src, encoding="utf-8")
    query_path = _REPO / "examples/benchmarks/neutroniumbomb_query.sql"
    env = bench_env(args.rows)

    setup_ms = []
    query_ms = []
    rps_vals = []
    for _ in range(args.runs):
        t0 = time.perf_counter()
        subprocess.run(
            [str(astral), "-m", "-O4", str(setup_path)],
            capture_output=True,
            text=True,
            env=env,
            cwd=str(_REPO),
            check=False,
        )
        setup_ms.append((time.perf_counter() - t0) * 1000.0)
        proc = subprocess.run(
            [
                str(astral),
                "-m",
                "-O4",
                "--time-sql-setup",
                str(setup_path),
                "--time-sql",
                str(query_path),
            ],
            capture_output=True,
            text=True,
            env=env,
            cwd=str(_REPO),
        )
        timing = parse_time_sql_output(proc.stdout + proc.stderr)
        query_ms.append(timing.execute_ms or timing.total_ms or 0.0)
        if timing.scanned_rows and timing.execute_ms and timing.execute_ms > 0:
            rps_vals.append(timing.scanned_rows / (timing.execute_ms / 1000.0))

    labels = ["setup (10× BULK)", "Q10 megafusion"]
    medians = [sorted(setup_ms)[len(setup_ms) // 2], sorted(query_ms)[len(query_ms) // 2]]
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(labels, medians, color=["#4c72b0", "#dd8452"])
    ax.set_ylabel("median ms")
    ax.set_title(f"neutroniumbomb ({args.rows:,} rows/table)")
    if rps_vals:
        med_rps = sorted(rps_vals)[len(rps_vals) // 2]
        ax.text(0.5, 0.95, f"Q10 ~{med_rps:.2e} rows/sec", transform=ax.transAxes, ha="center", va="top")
    fig.tight_layout()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=120)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
