#!/usr/bin/env python3
"""
Run AstralDB stress/torture SQL suites in isolated temp dirs and plot a timing histogram.

Each suite is executed via ``astraldb --time-sql FILE`` (same contract as
``scripts/run_torture_isolated.ps1``). Median wall time per suite is shown as a
log-scale bar chart.

Typical usage (from repo root, after building AstralDB):

  pip install -r scripts/benchmark-requirements.txt
  python scripts/stress_torture_histogram.py
  python scripts/stress_torture_histogram.py --runs 5 --output media/stress_hist.png
  python scripts/stress_torture_histogram.py --only torture,unhinged,bulk
"""

from __future__ import annotations

import argparse
import importlib.util
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

# (key, label, path relative to repo root)
SUITES: list[tuple[str, str, str]] = [
    ("torture", "torture_test\n(CI DML/WAL)", "examples/torture_test.sql"),
    ("advanced", "torture_advanced\n(geo/MathSci)", "examples/torture_advanced.sql"),
    ("unhinged", "torture_unhinged\n(bulk+VACUUM)", "examples/torture_unhinged.sql"),
    ("traffic", "stress_traffic\n(multi-commit)", "examples/stress_traffic.sql"),
    ("bench", "benchmark_torture\n(joins)", "examples/benchmark_torture.sql"),
    ("bulk", "stress_bulk_load\n(150k BULK)", "examples/benchmarks/stress_bulk_load.sql"),
    ("wal", "stress_wal_churn\n(WAL+DML)", "examples/benchmarks/stress_wal_churn.sql"),
    ("analytics", "stress_analytics_mix\n(16k join)", "examples/benchmarks/stress_analytics_mix.sql"),
]

TIME_RX = re.compile(
    r"parse\+compile_ms=([\d.]+)\s+execute_ms=([\d.]+)\s+total_ms=([\d.]+)"
)


def ensure_import(name: str, pip_name: str, no_fetch: bool) -> None:
    if importlib.util.find_spec(name) is not None:
        return
    if no_fetch:
        raise SystemExit(f"Missing package {name!r}; install or drop --no-fetch")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "--upgrade", pip_name])


def resolve_astral_executable(repo_root: Path) -> Optional[Path]:
    bin_dir = repo_root / "bin"
    cmake_release = repo_root / "build-cmake" / "Release"
    if platform.system() == "Windows":
        candidates = [
            bin_dir / "astraldb.exe",
            bin_dir / "astraldb",
            bin_dir / "AstralDB.exe",
            cmake_release / "astraldb.exe",
            repo_root / "build-ci" / "astraldb.exe",
        ]
    else:
        candidates = [
            bin_dir / "astraldb",
            bin_dir / "AstralDB",
            repo_root / "build-cmake" / "astraldb",
            repo_root / "build-ci" / "astraldb",
        ]
    for p in candidates:
        if p.is_file():
            return p.resolve()
    return None


def run_suite(
    astral: Path,
    sql_path: Path,
    opt_level: str,
    timeout_s: float,
) -> tuple[Optional[float], Optional[float], Optional[float], int]:
    """Returns (parse_compile_ms, execute_ms, total_ms, exit_code)."""
    try:
        with tempfile.TemporaryDirectory(prefix="astraldb_stress_") as tmp:
            cwd = Path(tmp)
            t0 = time.perf_counter()
            proc = subprocess.run(
                [str(astral), opt_level, "--time-sql", str(sql_path)],
                capture_output=True,
                text=True,
                timeout=timeout_s,
                check=False,
                cwd=str(cwd),
            )
            wall_ms = (time.perf_counter() - t0) * 1000.0
    except subprocess.TimeoutExpired:
        return None, None, None, 124
    except FileNotFoundError:
        return None, None, None, 127

    blob = proc.stderr + "\n" + proc.stdout
    m = TIME_RX.search(blob)
    if m:
        pc, ex, tot = map(float, m.groups())
        return pc, ex, tot, proc.returncode
    if proc.returncode == 0:
        return None, None, wall_ms, proc.returncode
    if blob.strip():
        print(f"  [{sql_path.name}] no timing line (rc={proc.returncode}):\n", blob[:1500], flush=True)
    return None, None, None, proc.returncode


def median_total(runs: list[tuple[Optional[float], Optional[float], Optional[float], int]]) -> Optional[float]:
    ok = [t[2] for t in runs if t[2] is not None]
    if not ok:
        return None
    return statistics.median(ok)


def plot_histogram(
    labels: list[str],
    totals_ms: list[Optional[float]],
    output: Path,
    title: str,
) -> None:
    import matplotlib.pyplot as plt

    xs: list[str] = []
    ys: list[float] = []
    for lab, ms in zip(labels, totals_ms):
        if ms is not None:
            xs.append(lab)
            ys.append(ms)
    if not ys:
        print("No successful timings to plot.", flush=True)
        return

    palette = ["#4c72b0", "#dd8452", "#55a868", "#c44e52", "#8c6bb1", "#937860", "#da8bc3", "#b0b0b0"]
    fig, ax = plt.subplots(figsize=(max(10, 1.35 * len(ys)), 5))
    colors = [palette[i % len(palette)] for i in range(len(ys))]
    bars = ax.bar(xs, ys, color=colors)
    ax.set_ylabel("Median wall time (ms)")
    ax.set_title(title)
    ax.set_yscale("log")
    ax.tick_params(axis="x", labelsize=8)
    for bar, v in zip(bars, ys):
        ax.annotate(
            f"{v:.0f} ms",
            xy=(bar.get_x() + bar.get_width() / 2, v),
            ha="center",
            va="bottom",
            fontsize=8,
        )
    fig.tight_layout()
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=150)
    print("Wrote", output.resolve(), flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--astral", type=Path, default=None, help="AstralDB CLI executable")
    ap.add_argument("--astral-opt", default="-O2", help="Optimization flag (default -O2)")
    ap.add_argument("--runs", type=int, default=3, help="Repeats per suite (median reported)")
    ap.add_argument("--timeout", type=float, default=900.0, help="Per-run timeout seconds")
    ap.add_argument("--output", type=Path, default=Path("media/stress_torture_histogram.png"))
    ap.add_argument(
        "--only",
        default="",
        help="Comma-separated suite keys to run (default: all). Keys: "
        + ", ".join(k for k, _, _ in SUITES),
    )
    ap.add_argument("--no-fetch", action="store_true", help="Do not pip-install matplotlib")
    args = ap.parse_args()

    repo_root = args.repo_root.resolve()
    astral = args.astral.resolve() if args.astral else resolve_astral_executable(repo_root)
    if astral is None:
        print("No AstralDB executable found; pass --astral PATH.", file=sys.stderr)
        return 2

    ensure_import("matplotlib", "matplotlib", args.no_fetch)

    only_keys: set[str] = set()
    if args.only.strip():
        only_keys = {s.strip().lower() for s in args.only.split(",") if s.strip()}

    selected: list[tuple[str, str, Path]] = []
    for key, label, rel in SUITES:
        if only_keys and key.lower() not in only_keys:
            continue
        path = (repo_root / rel).resolve()
        if not path.is_file():
            print(f"Skipping missing suite {key}: {path}", flush=True)
            continue
        selected.append((key, label, path))

    if not selected:
        print("No suites selected.", file=sys.stderr)
        return 2

    print(f"AstralDB: {astral}", flush=True)
    print(f"Running {len(selected)} suite(s), {args.runs} run(s) each …\n", flush=True)

    plot_labels: list[str] = []
    medians: list[Optional[float]] = []

    for key, label, sql_path in selected:
        print(f"== {key} :: {sql_path.name} ==", flush=True)
        runs: list[tuple[Optional[float], Optional[float], Optional[float], int]] = []
        for i in range(args.runs):
            pc, ex, tot, rc = run_suite(astral, sql_path, args.astral_opt, args.timeout)
            runs.append((pc, ex, tot, rc))
            detail = "FAILED" if tot is None else (
                f"compile={pc:.1f} exec={ex:.1f} total={tot:.1f} ms"
                if pc is not None and ex is not None
                else f"wall_total={tot:.1f} ms"
            )
            print(f"  run {i + 1}/{args.runs}: rc={rc}  {detail}", flush=True)
        med = median_total(runs)
        plot_labels.append(label)
        medians.append(med)
        print(f"  median total_ms={med}\n", flush=True)

    title = (
        "AstralDB stress & torture suites (median wall time, log scale)\n"
        f"{args.runs} run(s)/suite, {args.astral_opt}, isolated temp cwd"
    )
    plot_histogram(plot_labels, medians, args.output.resolve(), title)

    ok = sum(1 for m in medians if m is not None)
    print(f"Plotted {ok}/{len(medians)} suites.", flush=True)
    return 0 if ok == len(medians) else 1


if __name__ == "__main__":
    sys.exit(main())
