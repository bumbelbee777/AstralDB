#!/usr/bin/env python3
"""Sweep neutroniumbomb Q10 across scales; plot derived rows/sec vs dataset size (superlinear scaling)."""

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
DEFAULT_SCALES = [100_000, 1_000_000, 10_000_000, 100_000_000, 1_000_000_000]
SCYLLA_RECORD_RPS = 1.0e9


def bench_env(rows: int) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_PARALLEL", "1")
    env.setdefault("ASTRALDB_ASYNC_PRECOMPUTE", "1")
    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "1")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows, 10_000_000))
    if rows >= 1_000_000_000:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "0")
        env.setdefault("ASTRALDB_MEMORY_CAP_GB", "128")
    else:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    return env


def measure_q10(astral: Path, rows: int, runs: int, timeout: int) -> dict:
    setup_src = BULK_RX.sub(
        f"BULK {rows}",
        (_REPO / "examples/benchmarks/neutroniumbomb_setup.sql").read_text(encoding="utf-8"),
    )
    tmpdir = Path(tempfile.mkdtemp(prefix=f"nuke_scale_{rows}_"))
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup_src, encoding="utf-8")
    query_path = _REPO / "examples/benchmarks/neutroniumbomb_query.sql"
    env = bench_env(rows)
    exec_samples: list[float] = []
    rps_samples: list[float] = []
    scanned = rows * 10
    wall_samples: list[float] = []
    for _ in range(runs):
        t0 = time.perf_counter()
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
            timeout=timeout,
        )
        wall_samples.append((time.perf_counter() - t0) * 1000.0)
        if proc.returncode != 0:
            raise RuntimeError(f"rows={rows}: {(proc.stderr or proc.stdout)[-2000:]}")
        timing = parse_time_sql_output(proc.stdout + proc.stderr)
        if timing.scanned_rows:
            scanned = timing.scanned_rows
        if timing.execute_ms and timing.execute_ms > 0:
            exec_samples.append(timing.execute_ms)
            if timing.scanned_rows:
                rps_samples.append(timing.scanned_rows / (timing.execute_ms / 1000.0))
    exec_samples.sort()
    rps_samples.sort()
    wall_samples.sort()
    mid = len(rps_samples) // 2
    return {
        "rows_per_table": rows,
        "scanned_rows": scanned,
        "execute_ms": exec_samples[mid] if exec_samples else None,
        "rows_per_sec": rps_samples[mid] if rps_samples else None,
        "wall_ms": wall_samples[mid] if wall_samples else None,
    }


def plot_sweep(results: list[dict], output: Path, host_label: str) -> None:
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker

    rows = [r["rows_per_table"] for r in results]
    rps = [r["rows_per_sec"] for r in results if r.get("rows_per_sec")]
    if len(rps) != len(rows):
        raise RuntimeError("missing rows_per_sec in sweep results")

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

    ax1.loglog(rows, rps, "o-", color="#c44e52", linewidth=2.5, markersize=9, label="AstralDB Q10 megafusion")
    ax1.axhline(SCYLLA_RECORD_RPS, color="#6c757d", linestyle="--", linewidth=1.5, label="ScyllaDB Nov 2019 headline (1B/s, 83 nodes)")
    ax1.set_xlabel("rows per table (10 tables)")
    ax1.set_ylabel("derived rows / second")
    ax1.set_title("Throughput scales with dataset size")
    ax1.legend(loc="lower right", fontsize=8)
    ax1.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, _: f"{y:.0e}"))
    ax1.grid(True, which="both", alpha=0.3)

    ratios = [r / SCYLLA_RECORD_RPS for r in rps]
    ax2.semilogx(rows, ratios, "s-", color="#4c72b0", linewidth=2.5, markersize=9)
    ax2.set_xlabel("rows per table")
    ax2.set_ylabel("× vs Scylla headline")
    ax2.set_title(f"Gap vs distributed NoSQL record ({host_label})")
    ax2.grid(True, which="both", alpha=0.3)
    for x, y in zip(rows, ratios):
        ax2.annotate(f"{y:.0f}×", (x, y), textcoords="offset points", xytext=(0, 8), ha="center", fontsize=8)

    foot = (
        "Derived metric: scanned_rows / (execute_ms/1000) on lazy synthetic BULK megafusion (-m -O4). "
        "Not materialized full-table I/O; Scylla point is 83-node cluster press-release scan."
    )
    fig.text(0.5, 0.01, foot, ha="center", va="bottom", fontsize=8, wrap=True)
    fig.tight_layout(rect=(0, 0.06, 1, 1))
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=140)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument(
        "--scales",
        default=",".join(str(x) for x in DEFAULT_SCALES),
        help="Comma-separated rows/table (default: 100k..1B)",
    )
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=7200)
    ap.add_argument("--output", type=Path, default=_REPO / "media" / "neutroniumbomb_scale_sweep.png")
    ap.add_argument("--json", type=Path, default=_REPO / "media" / "neutroniumbomb_scale_sweep.json")
    ap.add_argument("--skip-plot", action="store_true")
    args = ap.parse_args()

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable.", file=sys.stderr)
        return 2

    scales = [int(x.strip()) for x in args.scales.split(",") if x.strip()]
    host = platform.node() or "local"
    machine = f"{host} ({platform.system()} {platform.machine()})"

    print(f"neutroniumbomb scale sweep on {machine}")
    print(f"scales: {', '.join(f'{s:,}' for s in scales)}  runs={args.runs}")
    print("-" * 72)

    results: list[dict] = []
    for rows in scales:
        print(f"  {rows:,} rows/table…", flush=True)
        t0 = time.perf_counter()
        try:
            row = measure_q10(astral.resolve(), rows, args.runs, args.timeout)
        except subprocess.TimeoutExpired:
            print(f"    TIMEOUT", file=sys.stderr)
            return 124
        elapsed = time.perf_counter() - t0
        results.append(row)
        rps = row.get("rows_per_sec")
        rps_s = f"{rps:.3e}" if rps else "?"
        ratio = rps / SCYLLA_RECORD_RPS if rps else 0
        print(
            f"    execute_ms={row.get('execute_ms')}  scanned={row.get('scanned_rows'):,}  "
            f"rows_per_sec={rps_s}  vs_scylla={ratio:.0f}×  wall={elapsed:.1f}s"
        )

    args.json.parent.mkdir(parents=True, exist_ok=True)
    payload = {"machine": machine, "scales": results}
    args.json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"json: {args.json}")

    if not args.skip_plot:
        try:
            plot_sweep(results, args.output, machine)
            print(f"plot: {args.output}")
        except ImportError:
            print("pip install matplotlib for plot", file=sys.stderr)
            return 2

    if len(results) >= 2:
        first = results[0].get("rows_per_sec") or 0
        last = results[-1].get("rows_per_sec") or 0
        if first > 0:
            print(f"scaling: {last / first:.0f}× throughput from {scales[0]:,} to {scales[-1]:,} rows/table")
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
