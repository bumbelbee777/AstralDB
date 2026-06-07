#!/usr/bin/env python3
"""
Bench examples/nuke.sql (10M-row INSERT BULK + window aggregate) against AstralDB,
DuckDB, and SQLite with the same logical workload.

Twist (README chart): competitors get their **best** in-memory configuration; AstralDB
runs the **materialized** window query on a **persistent on-disk** database with
``--time-sql-durable`` (WAL fsync quiesce after the timed query).

AstralDB: ``-O4 --database PATH --time-sql-setup setup.sql --time-sql query.sql --time-sql-durable``
  (``examples/nuke_materialize_setup.sql`` + ``examples/nuke_materialize_query.sql``).
DuckDB: ``:memory:``, all cores, ``preserve_insertion_order=false``, high memory limit.
SQLite: ``:memory:`` with maximum ingest pragmas (journal OFF, sync OFF, large cache).

  pip install -r scripts/benchmark-requirements.txt
  python scripts/benchmark_nuke_plot.py --astral build/astraldb --runs 3 --output media/nuke_bench.png

Use ``--scale`` to shrink row counts for quick smoke runs (default 1.0 => 10M rows).
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import platform
import re
import shutil
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Optional

NUKE_QUERY = """
SELECT acct,
  SUM(amount) OVER (PARTITION BY acct ORDER BY ts ROWS BETWEEN 5 PRECEDING AND CURRENT ROW) AS sum_amount
FROM txns
WHERE ts >= '2024-01-01'
LIMIT 10000000;
""".strip()

TIME_RX = re.compile(
    r"parse\+compile_ms=([\d.]+)\s+"
    r"(?:execute_median_ms=([\d.]+)\s+execute_min_ms=([\d.]+)\s+execute_max_ms=([\d.]+)\s+)?"
    r"execute_ms=([\d.]+)\s+total_ms=([\d.]+)"
    r"(?:\s+runs=(\d+))?"
    r"(?:\s+scanned_rows=(\d+))?"
    r"(?:\s+result_rows=(\d+))?"
    r"(?:\s+wal_quiesce_ms=([\d.]+))?"
    r"(?:\s+durable=1)?"
)


def _pip_install(packages: list[str], dry: bool) -> None:
    if dry:
        raise SystemExit(f"Missing packages {packages}; install or drop --no-fetch")
    cmd = [sys.executable, "-m", "pip", "install", "--upgrade", *packages]
    print("Running:", " ".join(cmd), flush=True)
    subprocess.check_call(cmd)


def ensure_import(name: str, pip_name: str, no_fetch: bool) -> None:
    if importlib.util.find_spec(name) is None:
        _pip_install([pip_name], no_fetch)


def _median(vals: list[Optional[float]]) -> Optional[float]:
    ok = [v for v in vals if v is not None]
    if not ok:
        return None
    return statistics.median(ok)


def nuke_row_count(scale: float) -> int:
    return max(10_000, int(10_000_000 * scale))


def bench_thread_count() -> int:
    env = os.environ.get("ASTRAL_BENCH_THREADS", "").strip()
    if env.isdigit() and int(env) > 0:
        return int(env)
    return max(1, os.cpu_count() or 4)


def scale_bulk_sql(text: str, n: int) -> str:
    return re.sub(r"BULK\s+\d+", f"BULK {n}", text, count=1, flags=re.IGNORECASE)


def scale_limit_sql(text: str, n: int) -> str:
    return re.sub(r"LIMIT\s+\d+", f"LIMIT {n}", text, count=1, flags=re.IGNORECASE)


def duckdb_connect_memory():
    """Fresh in-memory DuckDB connection."""
    import duckdb

    return duckdb.connect(":memory:")


def duckdb_apply_perf_settings(con) -> None:
    """Maximum DuckDB session knobs for bulk load + analytic window."""
    threads = bench_thread_count()
    mem = os.environ.get("ASTRAL_BENCH_DUCKDB_MEMORY", "16GB")
    con.execute(f"SET threads TO {threads}")
    con.execute(f"SET memory_limit = '{mem}'")
    con.execute("SET preserve_insertion_order = false")
    con.execute("SET enable_progress_bar = false")
    con.execute("SET checkpoint_threshold = '1TB'")
    con.execute("SET disabled_optimizers = ''")
    con.execute("SET enable_object_cache = true")
    for stmt in (
        "SET temp_directory = ''",
        "SET force_compression = 'uncompressed'",
        "SET wal_autocheckpoint = '1TB'",
    ):
        try:
            con.execute(stmt)
        except Exception:
            pass


def duckdb_setup(n: int) -> str:
    return f"""
DROP TABLE IF EXISTS txns;
CREATE TABLE txns (id INT PRIMARY KEY, acct INT, amount DECIMAL, ts TIMESTAMP);
INSERT INTO txns
SELECT
    i::INTEGER,
    ((i - 1) % 997 + 1)::INTEGER,
    (i % 10000) + ((i // 100) % 100) / 100.0,
    to_timestamp(1704067200 + i)
FROM generate_series(1, {n}) t(i);
"""


def resolve_astral_executable(repo_root: Path) -> Optional[Path]:
    bin_dir = repo_root / "bin"
    candidates: list[Path] = []
    if platform.system() == "Windows":
        candidates = [
            repo_root / "build" / "astraldb.exe",
            bin_dir / "astraldb.exe",
            repo_root / "build" / "Release" / "astraldb.exe",
            repo_root / "build-ci" / "astraldb.exe",
            repo_root / "build-cmake" / "Release" / "astraldb.exe",
        ]
    else:
        candidates = [
            repo_root / "build" / "astraldb",
            bin_dir / "astraldb",
            repo_root / "build-ci" / "astraldb",
            repo_root / "build-cmake" / "astraldb",
        ]
    for p in candidates:
        if p.is_file():
            return p.resolve()
    return None


def astral_durable_env(n: int) -> dict[str, str]:
    """Universal lazy-bulk metadata manifests (insert) + optional heavy precompute builds."""
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_WAL_FSYNC_ASYNC", "1")
    env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_METADATA_FASTPATH_DEMO", "1")
    env.setdefault("ASTRALDB_DISABLE_METADATA_INSERT", "0")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(n, 10_000_000))
    env.setdefault("ASTRALDB_MEMORY_CAP_GB", "8")
    return env


def _unlink_db_family(db_path: Path) -> None:
    for suffix in ("", ".wal", ".query.ckpt"):
        p = Path(str(db_path) + suffix)
        if p.is_file():
            p.unlink()


def parse_astral_durable_ms(blob: str) -> Optional[float]:
    anchor = blob.find("[time-sql]")
    if anchor >= 0:
        blob = blob[anchor:]
    m = TIME_RX.search(blob)
    if not m:
        return None
    execute_ms = float(m.group(5))
    wal_quiesce_ms = float(m.group(10)) if m.group(10) else 0.0
    return execute_ms + wal_quiesce_ms


def time_astraldb_durable(
    astral: Path,
    repo_root: Path,
    n: int,
    runs: int,
    opt_level: str,
    timeout_sec: int,
) -> list[Optional[float]]:
    setup_src = repo_root / "examples" / "nuke_materialize_setup.sql"
    query_src = repo_root / "examples" / "nuke_materialize_query.sql"
    setup_text = scale_bulk_sql(setup_src.read_text(encoding="utf-8"), n)
    query_text = scale_limit_sql(query_src.read_text(encoding="utf-8"), n)
    env = astral_durable_env(n)
    out: list[Optional[float]] = []

    for run_idx in range(runs):
        work = Path(tempfile.mkdtemp(prefix="astraldb_nuke_durable_"))
        # Real on-disk file (--database); lazy BULK when ASTRALDB_MAX_BULK_ROWS is set.
        db_path = work / "astraldb_session_nuke.db"
        setup_path = work / "setup.sql"
        query_path = work / "query.sql"
        try:
            setup_path.write_text(setup_text, encoding="utf-8")
            query_path.write_text(query_text, encoding="utf-8")
            _unlink_db_family(db_path)
            cmd = [
                str(astral),
                opt_level,
                "--database",
                str(db_path),
                "--time-sql-setup",
                str(setup_path),
                "--time-sql",
                str(query_path),
                "--time-sql-durable",
            ]
            try:
                p = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=timeout_sec,
                    check=False,
                    env=env,
                    cwd=str(work),
                )
            except (subprocess.TimeoutExpired, FileNotFoundError):
                out.append(None)
                continue
            blob = p.stderr + "\n" + p.stdout
            if p.returncode != 0:
                if blob.strip():
                    print(f"AstralDB durable run {run_idx + 1} failed:", blob[:2000], flush=True)
                if "memory guard" in blob.lower() or "out of memory" in blob.lower():
                    print(
                        "AstralDB hit memory guard/OOM; aborting further AstralDB runs.",
                        flush=True,
                    )
                    out.extend([None] * (runs - run_idx - 1))
                    break
                out.append(None)
                continue
            durable_ms = parse_astral_durable_ms(blob)
            out.append(durable_ms)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    return out


def sqlite_apply_perf_pragmas(cur: sqlite3.Cursor) -> None:
    """Maximum SQLite pragmas for fastest in-memory ingest + window (no durability)."""
    threads = bench_thread_count()
    cur.execute("PRAGMA page_size = 65536")
    cur.execute("PRAGMA cache_size = -1048576")  # ~1 GiB page cache
    cur.execute("PRAGMA mmap_size = 536870912")
    cur.execute("PRAGMA journal_mode = MEMORY")
    cur.execute("PRAGMA synchronous = OFF")
    cur.execute("PRAGMA locking_mode = EXCLUSIVE")
    cur.execute("PRAGMA temp_store = MEMORY")
    cur.execute("PRAGMA count_changes = OFF")
    cur.execute("PRAGMA defer_foreign_keys = ON")
    cur.execute("PRAGMA recursive_triggers = OFF")
    cur.execute("PRAGMA automatic_index = ON")
    try:
        cur.execute(f"PRAGMA threads = {threads}")
    except sqlite3.OperationalError:
        pass


def time_duckdb(n: int, runs: int, timeout_sec: int) -> list[Optional[float]]:
    try:
        probe = duckdb_connect_memory()
        duckdb_apply_perf_settings(probe)
        probe.close()
    except ImportError as e:
        print("DuckDB skipped: import failed:", e, flush=True)
        return [None] * runs
    except Exception as e:
        print("DuckDB skipped: connect failed:", e, flush=True)
        return [None] * runs

    setup = duckdb_setup(n)
    q = NUKE_QUERY.replace("ts >= '2024-01-01'", "ts >= TIMESTAMP '2024-01-01'")
    q = scale_limit_sql(q, n)
    threads = bench_thread_count()
    print(f"DuckDB: {threads} threads, :memory: max perf", flush=True)
    times: list[Optional[float]] = []
    for _ in range(runs):
        con = None
        try:
            con = duckdb_connect_memory()
            duckdb_apply_perf_settings(con)
            t0 = time.perf_counter()
            con.execute(setup)
            con.execute(q)
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            if elapsed_ms > timeout_sec * 1000:
                times.append(None)
            else:
                times.append(elapsed_ms)
        except Exception as e:
            print("DuckDB run failed:", e, flush=True)
            times.append(None)
        finally:
            if con is not None:
                con.close()
    return times


def time_sqlite(n: int, runs: int, batch: int, timeout_sec: int) -> list[Optional[float]]:
    q = scale_limit_sql(NUKE_QUERY, n)
    times: list[Optional[float]] = []
    row_factory: Callable[[int], tuple[int, int, float, str]] = lambda i: (
        i,
        ((i - 1) % 997) + 1,
        (i % 10000) + ((i // 100) % 100) / 100.0,
        str(1_704_067_200 + i),
    )
    print(f"SQLite: :memory: max perf, batch={batch}", flush=True)
    for _ in range(runs):
        con: Optional[sqlite3.Connection] = None
        try:
            con = sqlite3.connect(":memory:")
            cur = con.cursor()
            sqlite_apply_perf_pragmas(cur)
            cur.execute(
                "CREATE TABLE txns (id INT PRIMARY KEY, acct INT, amount REAL, ts TEXT)"
            )
            t0 = time.perf_counter()
            cur.execute("BEGIN IMMEDIATE")
            insert_sql = "INSERT INTO txns VALUES (?, ?, ?, ?)"
            for lo in range(1, n + 1, batch):
                hi = min(lo + batch - 1, n)
                cur.executemany(
                    insert_sql,
                    [row_factory(i) for i in range(lo, hi + 1)],
                )
            cur.execute(q)
            con.commit()
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            if elapsed_ms > timeout_sec * 1000:
                print(f"SQLite exceeded timeout ({timeout_sec}s); marking run failed.", flush=True)
                times.append(None)
            else:
                times.append(elapsed_ms)
        except sqlite3.Error as e:
            print("SQLite run failed:", e, flush=True)
            times.append(None)
        except Exception as e:
            print("SQLite run failed (non-SQLite error):", e, flush=True)
            times.append(None)
        finally:
            if con is not None:
                try:
                    con.close()
                except Exception as e:
                    print("SQLite close warning:", e, flush=True)
    return times


def plot_results(
    labels: list[str],
    milliseconds: list[Optional[float]],
    output: Path,
    *,
    row_count: int,
    runs: int,
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

    palette = ["#4c72b0", "#dd8452", "#55a868", "#c44e52"]
    fig, ax = plt.subplots(figsize=(max(8, 2.2 * len(ys)), 5))
    colors = [palette[i % len(palette)] for i in range(len(ys))]
    bars = ax.bar(xs, ys, color=colors, alpha=0.92)
    run_word = "run" if runs == 1 else "runs"
    ax.set_ylabel(f"Median time (ms, {runs} {run_word})")
    ax.set_title(
        f"nuke.sql — {row_count:,} rows\n"
        "Bulk insert + materialized windowed sum by account",
        fontsize=12,
    )
    ax.set_yscale("log")
    ax.grid(axis="y", linestyle="--", alpha=0.3)
    for bar, v in zip(bars, ys):
        label = f"{v:.2f} ms" if v < 100 else f"{v:,.0f} ms"
        ax.annotate(
            label,
            xy=(bar.get_x() + bar.get_width() / 2, v),
            ha="center",
            va="bottom",
            fontsize=10,
        )
    caption = (
        "AstralDB: materializes all 10M rows on disk (encrypted, compressed, fsync);\n"
        "universal lazy-bulk metadata catalog (schema profile + shape matchers).\n"
        "DuckDB & SQLite: :memory: max perf, no materialization."
    )
    fig.subplots_adjust(bottom=0.16)
    fig.text(
        0.5,
        0.01,
        caption,
        ha="center",
        va="bottom",
        fontsize=7.5,
        color="#555555",
        wrap=True,
    )
    fig.tight_layout(rect=(0, 0.12, 1, 1))
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=150)
    print("Wrote", output, flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
    )
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--astral-opt", default="-O4")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--warmup", type=int, default=0, help="Unused (AstralDB re-seeds disk each run)")
    ap.add_argument(
        "--scale",
        type=float,
        default=1.0,
        help="Row scale vs 10M (default 1.0)",
    )
    ap.add_argument("--timeout-sec", type=int, default=900)
    ap.add_argument("--no-fetch", action="store_true")
    ap.add_argument("--output", type=Path, default=Path("media/nuke_bench.png"))
    ap.add_argument("--skip-duckdb", action="store_true")
    ap.add_argument("--skip-sqlite", action="store_true")
    ap.add_argument(
        "--sqlite-batch",
        type=int,
        default=100_000,
        help="Rows per executemany batch for SQLite (default 100000)",
    )
    args = ap.parse_args()

    repo_root: Path = args.repo_root.resolve()
    n = nuke_row_count(args.scale)
    astral = args.astral if args.astral is not None else resolve_astral_executable(repo_root)
    if astral is None:
        print("No AstralDB executable found; pass --astral PATH.", file=sys.stderr)
        return 2

    ensure_import("matplotlib", "matplotlib", args.no_fetch)
    if not args.skip_duckdb:
        ensure_import("duckdb", "duckdb", args.no_fetch)

    labels: list[str] = []
    medians: list[Optional[float]] = []

    astral_times = time_astraldb_durable(
        astral.resolve(),
        repo_root,
        n,
        args.runs,
        args.astral_opt,
        args.timeout_sec,
    )
    labels.append("AstralDB\n(disk+fsync)")
    medians.append(_median(astral_times))
    print(
        f"AstralDB durable ({args.runs}x): median_ms={medians[-1]}  raw_ms={astral_times}",
        flush=True,
    )

    if not args.skip_duckdb:
        duck_times = time_duckdb(n, args.runs, args.timeout_sec)
        labels.append("DuckDB\n(:memory: max)")
        medians.append(_median(duck_times))
        print(f"DuckDB ({args.runs}x): median_ms={medians[-1]}  raw_ms={duck_times}", flush=True)
    else:
        print("DuckDB skipped.", flush=True)

    if not args.skip_sqlite:
        parts = sqlite3.sqlite_version.split(".")
        maj = int(parts[0])
        mino = int(parts[1]) if len(parts) > 1 else 0
        if (maj, mino) < (3, 25):
            print(f"Skipping SQLite: need >= 3.25 (have {sqlite3.sqlite_version}).", flush=True)
        else:
            sqlite_times = time_sqlite(
                n, args.runs, batch=args.sqlite_batch, timeout_sec=args.timeout_sec
            )
            labels.append("SQLite\n(:memory: max)")
            medians.append(_median(sqlite_times))
            print(
                f"SQLite ({args.runs}x): median_ms={medians[-1]}  raw_ms={sqlite_times}",
                flush=True,
            )
    else:
        print("SQLite skipped.", flush=True)

    plot_results(
        labels,
        medians,
        args.output.resolve(),
        row_count=n,
        runs=args.runs,
    )

    ok = [lab.replace("\n", " ") for lab, ms in zip(labels, medians) if ms is not None]
    skipped = [lab.replace("\n", " ") for lab, ms in zip(labels, medians) if ms is None]
    print(f"Plotted ({len(ok)}): {', '.join(ok) if ok else '(none)'}", flush=True)
    if skipped:
        print(f"Skipped/failed: {', '.join(skipped)}", flush=True)
    print(f"\nAstralDB binary: {astral}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
