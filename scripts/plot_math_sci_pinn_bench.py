#!/usr/bin/env python3
"""
Run PINN benchmark SQL harnesses and plot convergence + wall time.

  pip install -r scripts/benchmark-requirements.txt
  python scripts/gen_pinn_benchmark_sql.py
  python scripts/plot_math_sci_pinn_bench.py --astral build-release/Release/astraldb.exe
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


BENCHES = [
    ("tdse_1d", "examples/benchmarks/benchmark_pinn_tdse_1d.sql", "tdse_residual", 32, 8),
    ("ns_3d", "examples/benchmarks/benchmark_pinn_navier_stokes_3d.sql", "ns_div", 24, 8),
]


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
    if not bundle.is_file():
        raise FileNotFoundError(f"expected export at {bundle}")

    data = json.loads(bundle.read_text(encoding="utf-8"))
    tables = data.get("tables", {})
    bench = tables.get("bench_log", {}).get("rows", [])
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


def plot_results(
    metrics: Dict[str, Tuple[float, Dict[str, List[Tuple[int, float]]], int, int]],
    loss_tags: Dict[str, str],
    output: Path,
) -> None:
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    ax0 = axes[0]
    for name, (_, series, _, _) in metrics.items():
        tag = loss_tags[name]
        pts = series.get(tag, [])
        if not pts:
            continue
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        ax0.plot(xs, ys, marker="o", label=name)
    ax0.set_xlabel("epoch")
    ax0.set_ylabel("collocation MSE (center point)")
    ax0.set_title("PINN residual convergence")
    ax0.set_yscale("log")
    ax0.legend()
    ax0.grid(True, alpha=0.3)

    ax1 = axes[1]
    labels = []
    medians = []
    colors = ["#4C78A8", "#F58518"]
    for i, (name, (med_ms, _, pts_n, epochs)) in enumerate(metrics.items()):
        labels.append(f"{name}\n({pts_n} pts × {epochs} ep)")
        medians.append(med_ms)
    ax1.bar(labels, medians, color=colors[: len(labels)])
    ax1.set_ylabel("median wall time (ms)")
    ax1.set_title("PINN benchmark (PREDICT cache + SIMD)")

    fig.tight_layout()
    fig.savefig(output, dpi=150)
    print(f"Wrote {output}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Plot MathSci PINN benchmark convergence and timing.")
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--output", type=Path, default=Path("media/benchmark_math_sci_pinn_plot.png"))
    ap.add_argument("--regen-sql", action="store_true", help="Run scripts/gen_pinn_benchmark_sql.py first")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parents[1]
    if args.regen_sql:
        subprocess.run([sys.executable, str(repo / "scripts" / "gen_pinn_benchmark_sql.py")], check=True)

    astral = resolve_astral(args.astral, repo)
    loss_tags = {name: tag for name, _, tag, _, _ in BENCHES}

    metrics: Dict[str, Tuple[float, Dict[str, List[Tuple[int, float]]], int, int]] = {}
    with tempfile.TemporaryDirectory(prefix="pinn_bench_") as tmp:
        tmp_path = Path(tmp)
        for name, rel_sql, _, n_pts, n_ep in BENCHES:
            sql = repo / rel_sql
            db = tmp_path / f"{name}.db"
            print(f"Running {name} ({n_pts} collocation × {n_ep} epochs, {args.runs} runs)...", flush=True)
            median_ms, series = run_bench(astral, sql, db, args.runs)
            metrics[name] = (median_ms, series, n_pts, n_ep)
            n_residual = len(series.get(loss_tags[name], []))
            print(f"  median wall={median_ms:.1f} ms  logged epochs={n_residual}", flush=True)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        plot_results(metrics, loss_tags, args.output)
    except ImportError:
        print("matplotlib not installed; pip install matplotlib", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
