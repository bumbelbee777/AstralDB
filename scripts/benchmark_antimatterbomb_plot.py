#!/usr/bin/env python3

"""

Bench antimatterbomb (five-table INSERT BULK + OLAP/window/CUBE queries) on AstralDB.



  pip install -r scripts/benchmark-requirements.txt

  python scripts/benchmark_antimatterbomb_plot.py --astral build/astraldb --runs 3 \\

      --output media/antimatterbomb_bench.png



Default scale loads 2M rows per table (10M total) on ephemeral ``-m -O4``.

Use ``--scale 0.1`` for a quick smoke run (200k rows per table).

Use ``--with-cube`` to include the CUBE+join phase (Query 1 shape).

"""



from __future__ import annotations



import argparse

import importlib.util

import os

import platform

import re

import statistics

import subprocess

import sys

import tempfile

from pathlib import Path

from typing import Optional



_REPO = Path(__file__).resolve().parent.parent

if str(_REPO / "scripts") not in sys.path:

    sys.path.insert(0, str(_REPO / "scripts"))



from bench_timing_util import (  # noqa: E402

    resolve_astral_executable,

    run_time_sql,

)



DEFAULT_ROWS_PER_TABLE = 2_000_000

BULK_RX = re.compile(r"BULK\s+\d+", re.IGNORECASE)





def _pip_install(packages: list[str], dry: bool) -> None:

    if dry:

        raise SystemExit(f"Missing packages {packages}; install or drop --no-fetch")

    cmd = [sys.executable, "-m", "pip", "install", "--upgrade", *packages]

    print("Running:", " ".join(cmd), flush=True)

    subprocess.check_call(cmd)





def ensure_import(name: str, pip_name: str, no_fetch: bool) -> None:

    if importlib.util.find_spec(name) is None:

        _pip_install([pip_name], no_fetch)





def scaled_rows_per_table(scale: float) -> int:

    return max(10_000, int(DEFAULT_ROWS_PER_TABLE * scale))





def rewrite_bulk_counts(text: str, rows: int) -> str:

    return BULK_RX.sub(f"BULK {rows}", text)





def write_scaled_sql(src: Path, rows: int) -> Path:

    body = rewrite_bulk_counts(src.read_text(encoding="utf-8"), rows)

    tf = tempfile.NamedTemporaryFile(

        mode="w",

        suffix=".sql",

        delete=False,

        encoding="utf-8",

    )

    tf.write(body)

    tf.close()

    return Path(tf.name)





def bench_env() -> dict[str, str]:

    env = os.environ.copy()

    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")

    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "0")

    return env





def run_phase(

    astral: Path,

    sql_path: Path,

    opt: str,

    timeout_s: float,

    setup_path: Optional[Path],

    runs: int,

    warmup: int,

) -> Optional[float]:

    env = bench_env()

    times: list[float] = []

    for i in range(runs):

        t = run_time_sql(

            astral.resolve(),

            sql_path,

            opt,

            timeout_s,

            memory=True,

            warmup=warmup if i == 0 else 0,

            runs=1,

            setup_path=setup_path,

        )

        if t.stable_ms() is not None:

            times.append(t.stable_ms())

        print(f"    run {i + 1}/{runs}: {t.stable_ms()} ms", flush=True)

    return statistics.median(times) if times else None





def plot_results(

    labels: list[str],

    milliseconds: list[Optional[float]],

    output: Path,

    rows_per_table: int,

    scale: float,

) -> None:

    ensure_import("matplotlib", "matplotlib", no_fetch=False)

    import matplotlib.pyplot as plt



    xs: list[str] = []

    ys: list[float] = []

    for lab, ms in zip(labels, milliseconds):

        if ms is not None:

            xs.append(lab)

            ys.append(ms)



    if not ys:

        print("No timings to plot.", flush=True)

        return



    palette = ["#4c72b0", "#dd8452", "#55a868", "#c44e52", "#8172b3"]

    fig, ax = plt.subplots(figsize=(max(9, 2.2 * len(ys)), 5))

    colors = [palette[i % len(palette)] for i in range(len(ys))]

    bars = ax.bar(xs, ys, color=colors, alpha=0.92)

    total_rows = rows_per_table * 5

    ax.set_ylabel("Wall time (ms, median)")

    ax.set_title(

        "antimatterbomb — bulk load + OLAP / window / CUBE\n"

        f"{rows_per_table:,} rows/table ({total_rows:,} total); scale={scale}",

        fontsize=11,

    )

    ax.grid(axis="y", linestyle="--", alpha=0.3)

    for bar, v in zip(bars, ys):

        ax.annotate(

            f"{v:.0f} ms",

            xy=(bar.get_x() + bar.get_width() / 2, v),

            ha="center",

            va="bottom",

            fontsize=10,

            fontweight="bold",

        )

    fig.text(

        0.5,

        0.01,

        "AstralDB: ephemeral -m -O4; columnar join/window fast paths; shard spill optional",

        ha="center",

        fontsize=9,

        color="#444444",

    )

    fig.tight_layout(rect=(0, 0.04, 1, 1))

    output.parent.mkdir(parents=True, exist_ok=True)

    fig.savefig(output, dpi=150)

    print("Wrote", output, flush=True)





def main() -> int:

    ap = argparse.ArgumentParser(description=__doc__)

    ap.add_argument("--repo-root", type=Path, default=_REPO)

    ap.add_argument("--astral", type=Path, default=None)

    ap.add_argument("--astral-opt", default="-O4")

    ap.add_argument("--runs", type=int, default=3)

    ap.add_argument("--warmup", type=int, default=1)

    ap.add_argument("--scale", type=float, default=1.0)

    ap.add_argument("--with-cube", action="store_true", help="Run CUBE+join phase after main query")

    ap.add_argument("--timeout-sec", type=int, default=1800)

    ap.add_argument("--no-fetch", action="store_true")

    ap.add_argument("--output", type=Path, default=Path("media/antimatterbomb_bench.png"))

    args = ap.parse_args()



    repo_root = args.repo_root.resolve()

    rows = scaled_rows_per_table(args.scale)

    astral = args.astral if args.astral is not None else resolve_astral_executable(repo_root)

    if astral is None:

        print("No AstralDB executable; pass --astral PATH.", file=sys.stderr)

        return 2



    ensure_import("matplotlib", "matplotlib", args.no_fetch)



    setup_src = repo_root / "examples" / "benchmarks" / "antimatterbomb_setup.sql"

    query_src = repo_root / "examples" / "benchmarks" / "antimatterbomb_query.sql"

    cube_src = repo_root / "examples" / "benchmarks" / "antimatterbomb_cube.sql"

    if not setup_src.is_file() or not query_src.is_file():

        print("Missing antimatterbomb_setup.sql or antimatterbomb_query.sql", file=sys.stderr)

        return 2

    if args.with_cube and not cube_src.is_file():

        print("Missing antimatterbomb_cube.sql", file=sys.stderr)

        return 2



    setup_scaled = write_scaled_sql(setup_src, rows)

    query_scaled = write_scaled_sql(query_src, rows)

    cube_scaled = write_scaled_sql(cube_src, rows) if args.with_cube else None

    try:

        print(f"Seeding {rows:,} rows/table via setup SQL…", flush=True)

        seed = run_time_sql(

            astral.resolve(),

            setup_scaled,

            args.astral_opt,

            float(args.timeout_sec),

            memory=True,

            warmup=0,

            runs=1,

        )

        if seed.stable_ms() is None:

            print("Setup failed (no timing line); aborting.", file=sys.stderr)

            return 1

        print(f"Setup complete (~{seed.stable_ms():.0f} ms).", flush=True)



        labels = [f"Setup\n(5× BULK)"]

        medians: list[Optional[float]] = [seed.stable_ms()]



        print("Query phase (join+aggregate + window stack)…", flush=True)

        query_med = run_phase(

            astral,

            query_scaled,

            args.astral_opt,

            float(args.timeout_sec),

            setup_scaled,

            args.runs,

            args.warmup,

        )

        labels.append(f"Query\n(median {args.runs})")

        medians.append(query_med)



        if args.with_cube and cube_scaled is not None:

            print("CUBE phase (Query 1 shape)…", flush=True)

            cube_med = run_phase(

                astral,

                cube_scaled,

                args.astral_opt,

                float(args.timeout_sec),

                setup_scaled,

                args.runs,

                args.warmup,

            )

            labels.append(f"CUBE\n(median {args.runs})")

            medians.append(cube_med)



        plot_results(labels, medians, args.output.resolve(), rows, args.scale)

        print(f"\nAstralDB: {astral}", flush=True)

        print(f"Median query ms: {query_med}", flush=True)

        return 0 if query_med is not None else 1

    finally:

        setup_scaled.unlink(missing_ok=True)

        query_scaled.unlink(missing_ok=True)

        if cube_scaled is not None:

            cube_scaled.unlink(missing_ok=True)





if __name__ == "__main__":

    raise SystemExit(main())

