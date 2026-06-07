#!/usr/bin/env python3
"""Log-scale chart: AstralDB neutroniumbomb derived rows/sec vs ScyllaDB Nov 2019 record.

AstralDB metric: scanned_rows / (execute_ms/1000) on lazy synthetic BULK (Q10 megafusion).
ScyllaDB metric: materialized scan throughput on 83-node bare-metal cluster (press release).
These are not directly comparable workloads; the chart labels both methodologies explicitly.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
if str(_REPO / "scripts") not in sys.path:
    sys.path.insert(0, str(_REPO / "scripts"))

from bench_timing_util import parse_time_sql_output, resolve_astral_executable  # noqa: E402

BULK_RX = re.compile(r"BULK\s+\d+", re.IGNORECASE)

# ScyllaDB Nov 2019 benchmark (83× Packet n2.xlarge, 526B sensor points / 1y scan).
SCYLLA_RECORD_RPS = 1.0e9
SCYLLA_PERSISTENT_RPS = 969.0e6
SCYLLA_PEAK_RPS = 1.5e9

CHART_TITLE = (
    "ScyllaDB Nov 2019 Headline (In-Memory Reads) vs. AstralDB HTAP Query (With fsync)"
)
SCYLLA_BAR = "ScyllaDB Nov 2019\n(83 nodes · 34 TB RAM · in-memory scan)"
ASTRAL_DISK_BAR = "AstralDB HTAP Q10\n(Vivobook · 16 GB RAM · disk + fsync)"
ASTRAL_MMAP_BAR = "AstralDB HTAP Q10\n(Vivobook · 16 GB RAM · -m session)"
CHART_CAPTION = (
    "ScyllaDB: 83 high-end nodes, 34 TB RAM, materialized in-memory scan. "
    "AstralDB: one Vivobook, 16 GB RAM, disk with durable WAL fsync. "
    "Different workloads and hardware; derived lazy-scan rows/sec."
)


def bench_env(rows: int, *, disk_durable: bool = False) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "1")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_PARALLEL", "1")
    env.setdefault("ASTRALDB_ASYNC_PRECOMPUTE", "1")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows, 10_000_000))
    if rows >= 1_000_000_000:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "0")
        env.setdefault("ASTRALDB_MEMORY_CAP_GB", "128")
    else:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    if disk_durable:
        env["ASTRALDB_WAL_FSYNC_ASYNC"] = "1"
        env["ASTRALDB_WAL_ZERO_COPY"] = "1"
    return env


def measure_q10_rps(
    astral: Path, rows: int, runs: int = 3, *, disk_durable: bool = False, db_path: Path | None = None
) -> tuple[float, float, int, float | None]:
    setup_src = BULK_RX.sub(
        f"BULK {rows}",
        (_REPO / "examples/benchmarks/neutroniumbomb_demo_setup.sql").read_text(encoding="utf-8"),
    )
    tmpdir = Path(tempfile.mkdtemp(prefix="nuke_vs_scylla_"))
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup_src, encoding="utf-8")
    query_path = _REPO / "examples/benchmarks/neutroniumbomb_query.sql"
    env = bench_env(rows, disk_durable=disk_durable)
    rps_vals: list[float] = []
    exec_vals: list[float] = []
    wal_vals: list[float] = []
    scanned = 0
    if disk_durable and db_path is None:
        db_path = Path(tempfile.mkdtemp(prefix="astraldb_disk_plot_")) / "astraldb_disk_htap.db"
    for run_idx in range(runs):
        if disk_durable:
            cmd = [str(astral), "-O4", "--database", str(db_path)]
            if run_idx == 0:
                cmd += ["--time-sql-setup", str(setup_path)]
            cmd += ["--time-sql", str(query_path), "--time-sql-durable"]
        else:
            cmd = [
                str(astral),
                "-m",
                "-O4",
                "--time-sql-setup",
                str(setup_path),
                "--time-sql",
                str(query_path),
            ]
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            env=env,
            cwd=str(_REPO),
        )
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr or proc.stdout or "benchmark failed")
        timing = parse_time_sql_output(proc.stdout + proc.stderr)
        if timing.scanned_rows:
            scanned = timing.scanned_rows
        if timing.scanned_rows and timing.execute_ms and timing.execute_ms > 0:
            rps_vals.append(timing.scanned_rows / (timing.execute_ms / 1000.0))
            exec_vals.append(timing.execute_ms)
        if timing.wal_quiesce_ms is not None:
            wal_vals.append(timing.wal_quiesce_ms)
    if not rps_vals:
        raise RuntimeError("no rows/sec samples from --time-sql")
    rps_vals.sort()
    exec_vals.sort()
    wal_vals.sort()
    wal_med = wal_vals[len(wal_vals) // 2] if wal_vals else None
    return rps_vals[len(rps_vals) // 2], exec_vals[len(exec_vals) // 2], scanned, wal_med


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=1_000_000_000, help="Rows per table for live measure")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument(
        "--output",
        type=Path,
        default=_REPO / "media" / "neutroniumbomb_vs_scylla_bench.png",
    )
    ap.add_argument(
        "--skip-measure",
        action="store_true",
        help="Use --astral-rps instead of running benchmark",
    )
    ap.add_argument("--astral-rps", type=float, default=None, help="Override measured AstralDB rows/sec")
    ap.add_argument(
        "--disk-durable",
        action="store_true",
        help="Traditional disk: no -m, --database, durable WAL, zero-copy async fsync",
    )
    args = ap.parse_args()

    try:
        import matplotlib.pyplot as plt
        import matplotlib.ticker as ticker
    except ImportError:
        print("pip install matplotlib", file=sys.stderr)
        return 2

    host = platform.node() or "local"
    machine = f"{host} ({platform.system()} {platform.machine()})"
    wal_ms: float | None = None

    if args.astral_rps is not None:
        astral_rps = args.astral_rps
        exec_ms = 0.0
        scanned = int(args.rows) * 10
        if args.disk_durable:
            htap_json = _REPO / "media" / "htap_disk_os_bench.json"
            if htap_json.is_file():
                try:
                    disk = json.loads(htap_json.read_text(encoding="utf-8")).get("disk", {})
                    exec_ms = float(disk.get("execute_ms") or 0.0)
                    w = disk.get("wal_quiesce_ms")
                    wal_ms = float(w) if w is not None else None
                    if disk.get("scanned_rows"):
                        scanned = int(disk["scanned_rows"])
                except (json.JSONDecodeError, OSError, TypeError, ValueError):
                    pass
    elif args.skip_measure:
        print("--skip-measure requires --astral-rps", file=sys.stderr)
        return 2
    else:
        astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
        if astral is None or not astral.is_file():
            print("No astraldb executable.", file=sys.stderr)
            return 2
        print(f"Measuring Q10 at {args.rows:,} rows/table on {machine}…", flush=True)
        astral_rps, exec_ms, scanned, wal_ms = measure_q10_rps(
            astral.resolve(), args.rows, args.runs, disk_durable=args.disk_durable
        )
        print(
            f"  execute_ms={exec_ms:.3f}  scanned_rows={scanned:,}  rows_per_sec={astral_rps:.3e}"
            + (f"  wal_quiesce_ms={wal_ms:.3f}" if wal_ms is not None else "")
        )

    astral_label = ASTRAL_DISK_BAR if args.disk_durable else ASTRAL_MMAP_BAR
    labels = [SCYLLA_BAR, astral_label]
    values = [SCYLLA_RECORD_RPS, astral_rps]
    colors = ["#6c757d", "#c44e52"]

    fig, ax = plt.subplots(figsize=(9, 5))
    bars = ax.bar(labels, values, color=colors, width=0.55)
    ax.set_yscale("log")
    ax.set_ylabel("rows / second (log scale)")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, _: f"{y:.0e}"))
    ax.set_title(CHART_TITLE)

    for bar, val in zip(bars, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            val * 1.35,
            f"{val:.2e}",
            ha="center",
            va="bottom",
            fontsize=11,
            fontweight="bold",
        )

    fig.text(0.5, 0.02, CHART_CAPTION, ha="center", va="bottom", fontsize=9, wrap=True)
    fig.tight_layout(rect=(0, 0.08, 1, 1))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=140)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
