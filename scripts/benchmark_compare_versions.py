#!/usr/bin/env python3
"""Compare AstralDB benchmark medians (v1 vs v2) against README reference numbers."""

from __future__ import annotations

import argparse
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_timing_util import run_time_sql, robust_median  # noqa: E402

# README reference medians (ms) — update when README tables change
README_MS: dict[str, float] = {
    "sql92_unified": 15.5,
    "nuke": 38.7,
    "torture_test": 11.5,
    "torture_advanced": 13.5,
    "torture_unhinged": 22.1,
    "stress_traffic": 16.2,
    "benchmark_torture": 16.2,
    "stress_bulk_load": 16.2,
    "stress_wal_churn": 15.3,
    "stress_analytics_mix": 12.8,
    "pinn_tdse_1d": 64.9,
    "pinn_navier_stokes_3d": 63.5,
    "inference_nfp": 47.2,
}

STRESS_SUITES: list[tuple[str, str, str, str]] = [
    ("torture_test", "torture_test.sql", "examples/torture_test.sql", "-O2"),
    ("torture_advanced", "torture_advanced.sql", "examples/torture_advanced.sql", "-O2"),
    ("torture_unhinged", "torture_unhinged.sql", "examples/torture_unhinged.sql", "-O2"),
    ("stress_traffic", "stress_traffic.sql", "examples/stress_traffic.sql", "-O2"),
    ("benchmark_torture", "benchmark_torture.sql", "examples/benchmark_torture.sql", "-O2"),
    ("stress_bulk_load", "stress_bulk_load.sql", "examples/benchmarks/stress_bulk_load.sql", "-O2"),
    ("stress_wal_churn", "stress_wal_churn.sql", "examples/benchmarks/stress_wal_churn.sql", "-O2"),
    ("stress_analytics_mix", "stress_analytics_mix.sql", "examples/benchmarks/stress_analytics_mix.sql", "-O2"),
]

PINN_SUITES: list[tuple[str, str]] = [
    ("pinn_tdse_1d", "examples/benchmarks/benchmark_pinn_tdse_1d.sql"),
    ("pinn_navier_stokes_3d", "examples/benchmarks/benchmark_pinn_navier_stokes_3d.sql"),
]


def run_stress_suite(
    astral: Path,
    repo: Path,
    key: str,
    rel: str,
    opt: str,
    warmup: int,
    runs: int,
    timeout_s: float,
) -> Optional[float]:
    sql_path = (repo / rel).resolve()
    with tempfile.TemporaryDirectory(prefix="astraldb_cmp_") as tmp:
        timing = run_time_sql(
            astral, sql_path, opt, timeout_s, cwd=Path(tmp), warmup=warmup, runs=runs
        )
        return timing.stable_ms()


def run_sql92_unified(
    astral: Path, repo: Path, warmup: int, runs: int, timeout_s: float
) -> Optional[float]:
    # Delegate to benchmark_torture_plot builders
    sys.path.insert(0, str((repo / "scripts").resolve()))
    try:
        import benchmark_torture_plot as btp  # type: ignore
    except ImportError:
        return None
    unified = btp.load_unified_query(repo)
    nc, no, nl = 1000, 10_000, 50_000  # scale 0.001
    script = btp.astraldb_setup(nc, no, nl) + unified

    with tempfile.NamedTemporaryFile(mode="w", suffix=".sql", delete=False, encoding="utf-8") as tf:
        tf.write(script)
        path = Path(tf.name)
    try:
        with tempfile.TemporaryDirectory(prefix="astraldb_sql92_") as tmp:
            timing = run_time_sql(
                astral, path, "-O3", timeout_s, cwd=Path(tmp), warmup=warmup, runs=runs
            )
            return timing.stable_ms()
    finally:
        path.unlink(missing_ok=True)


def run_nuke(astral: Path, repo: Path, warmup: int, runs: int, timeout_s: float) -> Optional[float]:
    sql_path = repo / "examples" / "nuke.sql"
    if not sql_path.is_file():
        return None
    timing = run_time_sql(
        astral, sql_path, "-O4", timeout_s, memory=True, warmup=warmup, runs=runs
    )
    return timing.stable_ms()


def run_pinn(
    astral: Path, repo: Path, rel: str, warmup: int, runs: int, timeout_s: float
) -> Optional[float]:
    sql_path = (repo / rel).resolve()
    vals: list[float] = []

    def once() -> Optional[float]:
        with tempfile.TemporaryDirectory(prefix="astraldb_pinn_") as tmp:
            db = Path(tmp) / "bench.db"
            t0 = time.perf_counter()
            proc = subprocess.run(
                [str(astral), "--database", str(db), str(sql_path)],
                capture_output=True,
                text=True,
                timeout=timeout_s,
                check=False,
            )
            wall_ms = (time.perf_counter() - t0) * 1000.0
            return wall_ms if proc.returncode == 0 else None

    for _ in range(warmup):
        once()
    for _ in range(runs):
        v = once()
        if v is not None:
            vals.append(v)
    return robust_median(vals)


def run_inference_nfp(
    astral: Path, repo: Path, warmup: int, runs: int, timeout_s: float
) -> Optional[float]:
    rel = "examples/benchmarks/benchmark_math_sci_inference_nfp.sql"
    return run_pinn(astral, repo, rel, warmup, runs, timeout_s)


def fmt_ms(v: Optional[float]) -> str:
    if v is None:
        return "FAIL"
    if v >= 1000:
        return f"{v / 1000:.2f}s"
    return f"{v:.1f}"


def pct_change(new: Optional[float], old: Optional[float]) -> str:
    if new is None or old is None or old == 0:
        return "—"
    delta = (new - old) / old * 100.0
    if delta < -1:
        return f"{delta:+.0f}% (faster)"
    if delta > 1:
        return f"{delta:+.0f}% (slower)"
    return "~0%"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--v1", type=Path, default=Path("build/Release/astraldb.exe"))
    ap.add_argument("--v2", type=Path, default=Path("build/astraldb.exe"))
    ap.add_argument(
        "--runs",
        type=int,
        default=5,
        help="Timed runs per suite (--time-sql-runs for SQL; subprocess repeats for PINN)",
    )
    ap.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="Warmup runs before timing (in-process for --time-sql, subprocess for PINN)",
    )
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--skip-pinn", action="store_true")
    ap.add_argument("--skip-inference", action="store_true")
    ap.add_argument("--skip-nuke", action="store_true")
    ap.add_argument("--skip-sql92", action="store_true")
    args = ap.parse_args()

    repo = args.repo_root.resolve()
    v1 = args.v1.resolve()
    v2 = args.v2.resolve()
    for label, p in [("v1", v1), ("v2", v2)]:
        if not p.is_file():
            print(f"Missing {label} binary: {p}", file=sys.stderr)
            return 2

    results: dict[str, dict[str, Optional[float]]] = {}

    print(f"v1: {v1}", flush=True)
    print(f"v2: {v2}", flush=True)
    print(f"runs={args.runs}\n", flush=True)

    if not args.skip_sql92:
        print("== sql92_unified ==", flush=True)
        results["sql92_unified"] = {
            "v1": run_sql92_unified(v1, repo, args.warmup, args.runs, args.timeout),
            "v2": run_sql92_unified(v2, repo, args.warmup, args.runs, args.timeout),
        }
        print(f"  v1={results['sql92_unified']['v1']}  v2={results['sql92_unified']['v2']}\n", flush=True)

    if not args.skip_nuke:
        print("== nuke ==", flush=True)
        results["nuke"] = {
            "v1": run_nuke(v1, repo, args.warmup, args.runs, args.timeout),
            "v2": run_nuke(v2, repo, args.warmup, args.runs, args.timeout),
        }
        print(f"  v1={results['nuke']['v1']}  v2={results['nuke']['v2']}\n", flush=True)

    for key, _name, rel, opt in STRESS_SUITES:
        print(f"== {key} ==", flush=True)
        results[key] = {
            "v1": run_stress_suite(v1, repo, key, rel, opt, args.warmup, args.runs, args.timeout),
            "v2": run_stress_suite(v2, repo, key, rel, opt, args.warmup, args.runs, args.timeout),
        }
        print(f"  v1={results[key]['v1']}  v2={results[key]['v2']}\n", flush=True)

    if not args.skip_pinn:
        for key, rel in PINN_SUITES:
            print(f"== {key} ==", flush=True)
            results[key] = {
                "v1": run_pinn(v1, repo, rel, args.warmup, args.runs, args.timeout),
                "v2": run_pinn(v2, repo, rel, args.warmup, args.runs, args.timeout),
            }
            print(f"  v1={results[key]['v1']}  v2={results[key]['v2']}\n", flush=True)

    if not args.skip_inference:
        print("== inference_nfp ==", flush=True)
        results["inference_nfp"] = {
            "v1": run_inference_nfp(v1, repo, args.warmup, args.runs, args.timeout),
            "v2": run_inference_nfp(v2, repo, args.warmup, args.runs, args.timeout),
        }
        print(f"  v1={results['inference_nfp']['v1']}  v2={results['inference_nfp']['v2']}\n", flush=True)

    print("\n" + "=" * 88)
    print(f"{'Benchmark':<28} {'README':>10} {'v1.0':>10} {'v2.0':>10} {'v2 vs v1':>14} {'v2 vs README':>14}")
    print("=" * 88)
    for key in results:
        readme = README_MS.get(key)
        v1m = results[key].get("v1")
        v2m = results[key].get("v2")
        readme_s = fmt_ms(readme) if readme is not None else "—"
        vs_v1 = pct_change(v2m, v1m)
        vs_readme = pct_change(v2m, readme) if readme is not None else "—"
        print(
            f"{key:<28} {readme_s:>10} {fmt_ms(v1m):>10} {fmt_ms(v2m):>10} {vs_v1:>14} {vs_readme:>14}"
        )
    print("=" * 88)

    slower = []
    for key, row in results.items():
        v1m, v2m = row.get("v1"), row.get("v2")
        if v1m is not None and v2m is not None and v2m > v1m * 1.05:
            slower.append((key, v1m, v2m))
    if slower:
        print("\nSlower on v2.0 (>5% regression vs v1.0):")
        for key, v1m, v2m in slower:
            print(f"  - {key}: {fmt_ms(v1m)} -> {fmt_ms(v2m)}")
    else:
        print("\nNo suite regressed >5% vs v1.0 on this machine.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
