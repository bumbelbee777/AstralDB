#!/usr/bin/env python3
"""HTAP Q10 at 1B on traditional disk (--database, no -m): durable WAL + zero-copy async fsync."""

from __future__ import annotations

import argparse
import json
import os
import platform
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
SCYLLA_RECORD_RPS = 1.0e9

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


def disk_durable_env(rows: int) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_PARALLEL", "1")
    env.setdefault("ASTRALDB_ASYNC_PRECOMPUTE", "1")
    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "1")
    env["ASTRALDB_WAL_FSYNC_ASYNC"] = "1"
    env["ASTRALDB_WAL_ZERO_COPY"] = "1"
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows, 10_000_000))
    if rows >= 1_000_000_000:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "0")
        env.setdefault("ASTRALDB_MEMORY_CAP_GB", "128")
    else:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    return env


def measure_disk_q10(
    astral: Path,
    rows: int,
    runs: int,
    timeout: int,
    *,
    fresh_db: bool,
    db_path: Path,
) -> dict:
    setup_src = BULK_RX.sub(
        f"BULK {rows}",
        (_REPO / "examples/benchmarks/neutroniumbomb_setup.sql").read_text(encoding="utf-8"),
    )
    tmpdir = db_path.parent
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup_src, encoding="utf-8")
    query_path = _REPO / "examples/benchmarks/neutroniumbomb_query.sql"
    env = disk_durable_env(rows)
    if fresh_db:
        for suffix in ("", ".wal"):
            p = Path(str(db_path) + suffix)
            if p.is_file():
                p.unlink()

    exec_samples: list[float] = []
    wal_samples: list[float] = []
    rps_samples: list[float] = []
    scanned = rows * 10

    for i in range(runs):
        cmd = [
            str(astral),
            "-O4",
            "--database",
            str(db_path),
            "--time-sql-setup",
            str(setup_path),
            "--time-sql",
            str(query_path),
            "--time-sql-durable",
        ]
        if i > 0:
            cmd = [
                str(astral),
                "-O4",
                "--database",
                str(db_path),
                "--time-sql",
                str(query_path),
                "--time-sql-durable",
            ]
        t0 = time.perf_counter()
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            env=env,
            cwd=str(_REPO),
            timeout=timeout,
        )
        wall_ms = (time.perf_counter() - t0) * 1000.0
        if proc.returncode != 0:
            raise RuntimeError(f"disk bench failed: {(proc.stderr or proc.stdout)[-2500:]}")
        timing = parse_time_sql_output(proc.stdout + proc.stderr)
        if timing.scanned_rows:
            scanned = timing.scanned_rows
        if timing.execute_ms and timing.execute_ms > 0:
            exec_samples.append(timing.execute_ms)
            if timing.scanned_rows:
                rps_samples.append(timing.scanned_rows / (timing.execute_ms / 1000.0))
        if timing.wal_quiesce_ms is not None:
            wal_samples.append(timing.wal_quiesce_ms)
        if i == 0:
            first_wall = wall_ms

    exec_samples.sort()
    rps_samples.sort()
    wal_samples.sort()
    mid = len(rps_samples) // 2 if rps_samples else 0
    return {
        "mode": "disk_durable_zero_copy",
        "mmap": False,
        "rows_per_table": rows,
        "scanned_rows": scanned,
        "execute_ms": exec_samples[mid] if exec_samples else None,
        "wal_quiesce_ms": wal_samples[mid] if wal_samples else None,
        "rows_per_sec": rps_samples[mid] if rps_samples else None,
        "wall_ms_first_run": first_wall,
        "vs_scylla": (rps_samples[mid] / SCYLLA_RECORD_RPS) if rps_samples else None,
    }


def plot_disk_vs_scylla(result: dict, output: Path, mmap_rps: float | None) -> None:
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker

    disk_rps = result.get("rows_per_sec") or 0.0
    labels = [SCYLLA_BAR]
    values = [SCYLLA_RECORD_RPS]
    colors = ["#6c757d"]
    if mmap_rps and mmap_rps > 0:
        labels.append(ASTRAL_MMAP_BAR)
        values.append(mmap_rps)
        colors.append("#55a868")
    labels.append(ASTRAL_DISK_BAR)
    values.append(disk_rps)
    colors.append("#c44e52")

    fig, ax = plt.subplots(figsize=(10, 5.5))
    bars = ax.bar(labels, values, color=colors, width=0.52)
    ax.set_yscale("log")
    ax.set_ylabel("derived rows / second (log scale)")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, _: f"{y:.0e}"))
    ax.set_title(CHART_TITLE)

    for bar, val in zip(bars, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            val * 1.25,
            f"{val:.2e}",
            ha="center",
            va="bottom",
            fontsize=10,
            fontweight="bold",
        )

    fig.text(0.5, 0.02, CHART_CAPTION, ha="center", va="bottom", fontsize=9, wrap=True)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=140)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=1_000_000_000)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=7200)
    ap.add_argument("--json", type=Path, default=_REPO / "media" / "htap_disk_os_bench.json")
    ap.add_argument("--output", type=Path, default=_REPO / "media" / "htap_disk_os_vs_scylla.png")
    ap.add_argument(
        "--mmap-json",
        type=Path,
        default=_REPO / "media" / "neutroniumbomb_scale_sweep.json",
        help="Optional prior -m sweep JSON for comparison bar",
    )
    args = ap.parse_args()

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable.", file=sys.stderr)
        return 2

    host = platform.node() or "local"
    machine = f"{host} ({platform.system()} {platform.machine()})"
    tmpdir = Path(tempfile.mkdtemp(prefix="astraldb_disk_bench_"))
    db_path = tmpdir / "astraldb_disk_htap.db"

    print(f"HTAP disk OS benchmark — {args.rows:,} rows/table on {machine}")
    print("no -m | --database | durable WAL | zero-copy async batched fsync")
    print("-" * 72)

    result = measure_disk_q10(
        astral.resolve(),
        args.rows,
        args.runs,
        args.timeout,
        fresh_db=True,
        db_path=db_path,
    )
    result["machine"] = machine

    mmap_rps = None
    if args.mmap_json.is_file():
        try:
            prior = json.loads(args.mmap_json.read_text(encoding="utf-8"))
            for row in prior.get("scales", []):
                if row.get("rows_per_table") == args.rows and row.get("rows_per_sec"):
                    mmap_rps = float(row["rows_per_sec"])
                    break
        except (json.JSONDecodeError, OSError):
            pass

    payload = {"machine": machine, "disk": result}
    if mmap_rps:
        payload["mmap_rows_per_sec"] = mmap_rps

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"json: {args.json}")

    rps = result.get("rows_per_sec")
    rps_s = f"{rps:.3e}" if rps else "?"
    print(
        f"  execute_ms={result.get('execute_ms')}  wal_quiesce_ms={result.get('wal_quiesce_ms')}  "
        f"scanned={result.get('scanned_rows'):,}  rows_per_sec={rps_s}  "
        f"vs_scylla={result.get('vs_scylla', 0):.0f}×"
    )

    try:
        plot_disk_vs_scylla(result, args.output, mmap_rps)
        print(f"plot: {args.output}")
    except ImportError:
        print("pip install matplotlib for plot", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
