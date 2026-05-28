#!/usr/bin/env python3
"""
Run MCTS / Bayesian / NFP benchmark SQL and plot convergence + wall time.

  python scripts/gen_inference_nfp_benchmark_sql.py
  python scripts/plot_math_sci_inference_nfp_bench.py --astral build-release/Release/astraldb.exe
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

BENCH = ("inference_nfp", "examples/benchmarks/benchmark_math_sci_inference_nfp.sql", "nfp_mean_gap")


def resolve_astral(explicit: Optional[Path], repo: Path) -> Path:
    if explicit and explicit.is_file():
        return explicit
    for cand in (
        repo / "bin" / "astraldb.exe",
        repo / "build-release" / "Release" / "astraldb.exe",
        repo / "build" / "astraldb.exe",
    ):
        if cand.is_file():
            return cand
    raise FileNotFoundError("astraldb executable not found; pass --astral")


def run_bench(astral: Path, sql_path: Path, db_path: Path, runs: int) -> Tuple[float, Dict[str, List[Tuple[int, float]]]]:
    times: List[float] = []
    for _ in range(runs):
        if db_path.exists():
            db_path.unlink()
        wal = Path(str(db_path) + ".wal")
        if wal.exists():
            wal.unlink()
        t0 = time.perf_counter()
        proc = subprocess.run(
            [str(astral), "--database", str(db_path), str(sql_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        wall_ms = (time.perf_counter() - t0) * 1000.0
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr or proc.stdout or f"astraldb failed on {sql_path}")
        times.append(wall_ms)

    bundle = db_path.parent / f"{db_path.stem}_bundle.json"
    if bundle.exists():
        bundle.unlink()
    proc = subprocess.run(
        [str(astral), "--database", str(db_path), "--export-bundle", str(bundle), "--export-format", "json"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr or proc.stdout or "export-bundle failed")

    data = json.loads(bundle.read_text(encoding="utf-8"))
    bench = data.get("tables", {}).get("bench_log", {}).get("rows", [])
    series: Dict[str, List[Tuple[int, float]]] = {}

    def field(row: dict, name: str) -> str:
        for k, v in row.items():
            if k.lower() == name.lower():
                return str(v) if v is not None else ""
        return ""

    for row in bench:
        tag = field(row, "tag")
        try:
            epoch = int(field(row, "epoch") or 0)
            loss_s = field(row, "loss")
            if not loss_s or loss_s[0] in "LTMC":
                continue
            loss = float(loss_s)
        except (TypeError, ValueError):
            continue
        series.setdefault(tag, []).append((epoch, loss))
    for tag in series:
        series[tag].sort(key=lambda t: t[0])
    return statistics.median(times), series


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--output", type=Path, default=Path("media/benchmark_math_sci_inference_nfp_plot.png"))
    ap.add_argument("--regen-sql", action="store_true")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parents[1]
    if args.regen_sql:
        subprocess.run([sys.executable, str(repo / "scripts" / "gen_inference_nfp_benchmark_sql.py")], check=True)

    astral = resolve_astral(args.astral, repo)
    name, rel_sql, loss_tag = BENCH
    sql = repo / rel_sql

    with tempfile.TemporaryDirectory(prefix="nfp_bench_") as tmp:
        db = Path(tmp) / f"{name}.db"
        print(f"Running {name} ({args.runs} runs)...", flush=True)
        median_ms, series = run_bench(astral, sql, db, args.runs)
        print(f"  median wall={median_ms:.1f} ms  epochs={len(series.get(loss_tag, []))}", flush=True)

    try:
        import matplotlib.pyplot as plt

        fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))
        pts = series.get(loss_tag, [])
        if pts:
            axes[0].plot([p[0] for p in pts], [p[1] for p in pts], marker="o", color="#4C78A8")
        axes[0].set_xlabel("epoch")
        axes[0].set_ylabel("mean-gap proxy")
        axes[0].set_title("Neural macro Fokker–Planck convergence")
        axes[0].set_yscale("log")
        axes[0].grid(True, alpha=0.3)

        axes[1].bar(["MCTS+Bayes+NFP"], [median_ms], color="#F58518")
        axes[1].set_ylabel("median wall time (ms)")
        axes[1].set_title("Combined inference benchmark")

        fig.tight_layout()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(args.output, dpi=150)
        print(f"Wrote {args.output}")
    except ImportError:
        print("matplotlib not installed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
