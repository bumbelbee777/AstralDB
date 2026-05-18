#!/usr/bin/env python3
"""
Bench a unified analytic query (examples/benchmarks/benchmark_torture_unified.sql) against
AstralDB, DuckDB, MySQL, and SQLite with the same row counts and the same logical SQL.

Typical AstralDB build (from repo root):

  pip install -r scripts/benchmark-requirements.txt
  # optional for MySQL: pip install pymysql

AstralDB is timed via ``astraldb --time-sql FILE`` on a generated script (DDL +
batched INSERTs + unified query). DuckDB, MySQL, and SQLite use schema-equivalent
DDL and the identical query text from ``examples/benchmarks/benchmark_torture_unified.sql``.
SQLite uses the stdlib ``sqlite3`` driver and the same batched INSERT DDL as AstralDB.

Automated MySQL: run ``powershell -File scripts/run_benchmark_with_mysql.ps1`` (add ``-RestartDockerDesktop``
if ``docker info`` returns HTTP 500 for every API). On Unix: ``scripts/run_benchmark_with_mysql.sh``.
See ``scripts/docker/mysql-benchmark-compose.yml``.
For an existing server, set ``MYSQL_*`` env vars or pass ``--mysql-*``. If PyMySQL is
missing or the server is unreachable, that engine is skipped (median None). Use
``--skip-mysql`` to omit it.

Use ``--skip-duckdb`` / ``--skip-sqlite`` to omit embedded engines.

Executable resolution (Windows): ``bin/astraldb.exe``, ``bin/AstralDB.exe`` (legacy),
then ``build-cmake/Release/astraldb.exe``.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import platform
import sqlite3
import re
import textwrap
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Optional

# ---------------------------------------------------------------------------
# Dependency bootstrap (PyPI = "from the web")
# ---------------------------------------------------------------------------


def _pip_install(packages: list[str], dry: bool) -> None:
    if dry:
        raise SystemExit(f"Missing packages {packages}; install or drop --no-fetch")
    cmd = [sys.executable, "-m", "pip", "install", "--upgrade", *packages]
    print("Running:", " ".join(cmd), flush=True)
    subprocess.check_call(cmd)


def ensure_import(name: str, pip_name: str, no_fetch: bool) -> None:
    if importlib.util.find_spec(name) is None:
        _pip_install([pip_name], no_fetch)


# ---------------------------------------------------------------------------
# Unified query (same file as examples/benchmarks/benchmark_torture_unified.sql)
# ---------------------------------------------------------------------------


def load_unified_query(repo_root: Path) -> str:
    path = repo_root / "examples" / "benchmarks" / "benchmark_torture_unified.sql"
    text = path.read_text(encoding="utf-8")
    # Strip leading comments so the parser sees SQL first (AstralDB / others).
    lines = text.splitlines()
    while lines and (lines[0].strip().startswith("--") or not lines[0].strip()):
        lines.pop(0)
    body = textwrap.dedent("\n".join(lines)).strip()
    return body + "\n"


# ---------------------------------------------------------------------------
# SQL builders (portable workload; scale matches historical torture targets × --scale)
# ---------------------------------------------------------------------------


def duckdb_setup(nc: int, no: int, nl: int) -> str:
    return f"""
DROP TABLE IF EXISTS line_items;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customers;

CREATE TABLE customers AS
SELECT i::INTEGER AS id, ('n' || i)::VARCHAR AS name, TIMESTAMP '2026-06-01 00:00:00' AS created_at
FROM generate_series(1, {nc}) t(i);

CREATE TABLE orders AS
SELECT i::INTEGER AS id, ((i - 1) % {nc})::INTEGER + 1 AS customer_id
FROM generate_series(1, {no}) t(i);

CREATE TABLE line_items AS
SELECT i::INTEGER AS id,
       ((i - 1) % {no})::INTEGER + 1 AS order_id,
       ((i % 997) + 1)::DOUBLE AS amount
FROM generate_series(1, {nl}) t(i);
"""


def mysql_setup(nc: int, no: int, nl: int) -> str:
    # Recursive CTEs (MySQL 8+); depth must exceed max row count.
    depth = max(nc, no, nl) + 10
    return f"""
SET SESSION cte_max_recursion_depth = {depth};
DROP TABLE IF EXISTS line_items;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customers;

CREATE TABLE customers (
    id INT PRIMARY KEY,
    name VARCHAR(256) NOT NULL,
    created_at VARCHAR(32) NOT NULL
);
INSERT INTO customers
WITH RECURSIVE seq(n) AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM seq WHERE n < {nc})
SELECT n, CONCAT('n', n), '2026-06-01' FROM seq;

CREATE TABLE orders (
    id INT PRIMARY KEY,
    customer_id INT NOT NULL
);
INSERT INTO orders
WITH RECURSIVE seq(n) AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM seq WHERE n < {no})
SELECT n, ((n - 1) % {nc}) + 1 FROM seq;

CREATE TABLE line_items (
    id INT PRIMARY KEY,
    order_id INT NOT NULL,
    amount DOUBLE NOT NULL
);
INSERT INTO line_items
WITH RECURSIVE seq(n) AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM seq WHERE n < {nl})
SELECT n, ((n - 1) % {no}) + 1, ((n % 997) + 1) * 1.0 FROM seq;
"""


def astraldb_setup(nc: int, no: int, nl: int, batch: int = 800) -> str:
    """DDL + batched INSERT … VALUES (AstralDB has no server-side generate_series)."""

    def batched_values(
        label: str, start: int, end: int, row_fn: Callable[[int], str],
    ) -> list[str]:
        out: list[str] = []
        for lo in range(start, end + 1, batch):
            hi = min(lo + batch - 1, end)
            parts = [row_fn(i) for i in range(lo, hi + 1)]
            out.append(f"INSERT INTO {label} VALUES " + ",".join(parts) + ";")
        return out

    lines: list[str] = [
        "DROP TABLE IF EXISTS line_items;",
        "DROP TABLE IF EXISTS orders;",
        "DROP TABLE IF EXISTS customers;",
        "CREATE TABLE customers (id INT PRIMARY KEY, name TEXT, created_at TEXT);",
        "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT);",
        "CREATE TABLE line_items (id INT PRIMARY KEY, order_id INT, amount DOUBLE);",
    ]
    lines += batched_values(
        "customers",
        1,
        nc,
        lambda i: f"({i},'n{i}','2026-06-01')",
    )
    lines += batched_values(
        "orders",
        1,
        no,
        lambda i: f"({i},{((i - 1) % nc) + 1})",
    )
    lines += batched_values(
        "line_items",
        1,
        nl,
        lambda i: f"({i},{((i - 1) % no) + 1},{((i % 997) + 1) * 1.0})",
    )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Timers
# ---------------------------------------------------------------------------

TimeMs = tuple[Optional[float], Optional[float], Optional[float]]  # parse, exec, total


def time_astraldb(astral: Path, sql_text: str, runs: int, opt_level: str) -> list[TimeMs]:
    out: list[TimeMs] = []
    rx = re.compile(
        r"parse\+compile_ms=([\d.]+)\s+execute_ms=([\d.]+)\s+total_ms=([\d.]+)"
    )
    for _ in range(runs):
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                suffix=".sql",
                delete=False,
                encoding="utf-8",
            ) as tf:
                tf.write(sql_text)
                tmp_path = tf.name
            try:
                t0 = time.perf_counter()
                p = subprocess.run(
                    [str(astral), "--time-sql", tmp_path, opt_level],
                    capture_output=True,
                    text=True,
                    timeout=600,
                    check=False,
                )
                wall_ms = (time.perf_counter() - t0) * 1000.0
            finally:
                Path(tmp_path).unlink(missing_ok=True)
        except (subprocess.TimeoutExpired, FileNotFoundError):
            out.append((None, None, None))
            continue
        blob = p.stderr + "\n" + p.stdout
        m = rx.search(blob)
        if m:
            parse_compile_ms, exec_ms, total_ms = map(float, m.groups())
            out.append((parse_compile_ms, exec_ms, total_ms))
        elif p.returncode == 0:
            # Some Windows builds attach no console to captured streams; use wall time.
            out.append((None, None, wall_ms))
        else:
            if blob.strip():
                print("AstralDB stderr/stdout (no timing line):", blob[:2000], flush=True)
            out.append((None, None, None))
    return out


def _median(vals: list[Optional[float]]) -> Optional[float]:
    ok = [v for v in vals if v is not None]
    if not ok:
        return None
    return statistics.median(ok)


def _split_sql_statements(sql: str) -> list[str]:
    parts: list[str] = []
    for raw in sql.split(";"):
        s = raw.strip()
        if s:
            parts.append(s + ";")
    return parts


def time_duckdb(nc: int, no: int, nl: int, unified: str, runs: int) -> list[Optional[float]]:
    try:
        import duckdb
    except ImportError as e:
        print("DuckDB skipped: import failed:", e, flush=True)
        return [None] * runs

    setup = duckdb_setup(nc, no, nl)
    q = unified
    times: list[Optional[float]] = []
    for _ in range(runs):
        con = None
        try:
            con = duckdb.connect(":memory:")
            t0 = time.perf_counter()
            con.execute(setup)
            con.execute(q)
            times.append((time.perf_counter() - t0) * 1000.0)
        except Exception as e:
            print("DuckDB run failed:", e, flush=True)
            times.append(None)
        finally:
            if con is not None:
                con.close()
    return times


def time_mysql(
    host: str,
    port: int,
    user: str,
    password: str,
    database: str,
    nc: int,
    no: int,
    nl: int,
    unified: str,
    runs: int,
) -> list[Optional[float]]:
    try:
        import pymysql
    except ImportError as e:
        print("MySQL skipped: PyMySQL import failed:", e, flush=True)
        return [None] * runs

    setup = mysql_setup(nc, no, nl)
    q = unified
    times: list[Optional[float]] = []
    for _ in range(runs):
        conn = None
        try:
            conn = pymysql.connect(
                host=host,
                port=port,
                user=user,
                password=password,
                database=database,
                connect_timeout=10,
                read_timeout=600,
                write_timeout=600,
            )
            conn.ping(reconnect=False)
        except Exception as e:
            print("MySQL connect failed:", e, flush=True)
            times.append(None)
            continue
        try:
            t0 = time.perf_counter()
            with conn.cursor() as cur:
                for stmt in _split_sql_statements(setup):
                    cur.execute(stmt)
                cur.execute(q)
            conn.commit()
            times.append((time.perf_counter() - t0) * 1000.0)
        except Exception as e:
            print("MySQL run failed:", e, flush=True)
            times.append(None)
        finally:
            if conn is not None:
                try:
                    conn.close()
                except Exception as e:
                    print("MySQL close warning:", e, flush=True)
    return times


def time_sqlite(nc: int, no: int, nl: int, unified: str, runs: int, batch: int = 800) -> list[Optional[float]]:
    """
    Same bulk DDL as AstralDB (INSERT … VALUES batches); unified query unchanged.
    Uses an in-memory DB with pragmas tuned for load speed (not durability).
    """
    setup = astraldb_setup(nc, no, nl, batch)
    times: list[Optional[float]] = []
    for _ in range(runs):
        con: Optional[sqlite3.Connection] = None
        try:
            con = sqlite3.connect(":memory:")
            cur = con.cursor()
            # Large recursive INSERT scripts are not used; keep defaults. Speed up bulk load.
            cur.execute("PRAGMA journal_mode = MEMORY")
            cur.execute("PRAGMA synchronous = OFF")
            cur.execute("PRAGMA temp_store = MEMORY")
            t0 = time.perf_counter()
            for stmt in _split_sql_statements(setup):
                cur.execute(stmt)
            cur.execute(unified.strip())
            con.commit()
            times.append((time.perf_counter() - t0) * 1000.0)
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


# ---------------------------------------------------------------------------
# Plot
# ---------------------------------------------------------------------------


def plot_results(
    labels: list[str],
    milliseconds: list[Optional[float]],
    output: Path,
    title: str,
) -> None:
    ensure_import("matplotlib", "matplotlib", no_fetch=False)
    import matplotlib.pyplot as plt

    xs = []
    ys = []
    for lab, ms in zip(labels, milliseconds):
        if ms is not None:
            xs.append(lab)
            ys.append(ms)

    if not ys:
        print("No timings to plot.", flush=True)
        return

    palette = ["#4c72b0", "#dd8452", "#55a868", "#c44e52", "#8c6bb1"]
    fig, ax = plt.subplots(figsize=(max(8, 2 * len(ys)), 4))
    colors = [palette[i % len(palette)] for i in range(len(ys))]
    bars = ax.bar(xs, ys, color=colors)
    ax.set_ylabel("Wall time (ms, median)")
    ax.set_title(title)
    ax.set_yscale("log")
    for bar, v in zip(bars, ys):
        ax.annotate(
            f"{v:.1f}",
            xy=(bar.get_x() + bar.get_width() / 2, v),
            ha="center",
            va="bottom",
            fontsize=9,
        )
    fig.tight_layout()
    fig.savefig(output, dpi=150)
    print("Wrote", output, flush=True)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def resolve_astral_executable(repo_root: Path) -> Optional[Path]:
    bin_dir = repo_root / "bin"
    cmake_release = repo_root / "build-cmake" / "Release"
    candidates: list[Path] = []
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="AstralDB repository root (default: parent of scripts/)",
    )
    ap.add_argument(
        "--astral",
        type=Path,
        default=None,
        help="Path to AstralDB CLI executable (default: first match under bin/)",
    )
    ap.add_argument(
        "--astral-opt",
        default="-O3",
        help="Optimization flag passed to AstralDB (default: -O3)",
    )
    ap.add_argument("--runs", type=int, default=3, help="Repeat each engine (median reported)")
    ap.add_argument(
        "--scale",
        type=float,
        default=0.001,
        help="Scale factor vs 1e6 / 1e7 / 5e7 row targets (default 0.001 => 1k / 10k / 50k)",
    )
    ap.add_argument("--no-fetch", action="store_true", help="Do not pip-install missing Python deps")
    ap.add_argument("--output", type=Path, default=Path("benchmark_torture_plot.png"))
    ap.add_argument(
        "--skip-mysql",
        action="store_true",
        help="Do not benchmark MySQL",
    )
    ap.add_argument(
        "--skip-duckdb",
        action="store_true",
        help="Do not benchmark DuckDB",
    )
    ap.add_argument(
        "--skip-sqlite",
        action="store_true",
        help="Do not benchmark SQLite (stdlib sqlite3)",
    )
    ap.add_argument("--mysql-host", default=os.environ.get("MYSQL_HOST", "127.0.0.1"))
    ap.add_argument("--mysql-port", type=int, default=int(os.environ.get("MYSQL_PORT", "3306")))
    ap.add_argument("--mysql-user", default=os.environ.get("MYSQL_USER", "root"))
    ap.add_argument("--mysql-password", default=os.environ.get("MYSQL_PASSWORD", ""))
    ap.add_argument("--mysql-database", default=os.environ.get("MYSQL_DATABASE", "test"))
    args = ap.parse_args()

    repo_root: Path = args.repo_root.resolve()
    unified = load_unified_query(repo_root)
    astral = args.astral if args.astral is not None else resolve_astral_executable(repo_root)
    if astral is None:
        print(
            "No AstralDB executable found under bin/ "
            "(expected astraldb or AstralDB.exe under bin/). "
            "Pass --astral PATH.",
            file=sys.stderr,
        )
        return 2

    sc = args.scale
    nc = max(100, int(1_000_000 * sc))
    no = max(1_000, int(10_000_000 * sc))
    nl = max(5_000, int(50_000_000 * sc))

    ensure_import("matplotlib", "matplotlib", args.no_fetch)

    if not args.skip_duckdb:
        ensure_import("duckdb", "duckdb", args.no_fetch)

    full_astral_script = astraldb_setup(nc, no, nl) + unified

    labels: list[str] = []
    medians: list[Optional[float]] = []

    # AstralDB
    astral_series = time_astraldb(astral, full_astral_script, args.runs, args.astral_opt)
    labels.append("AstralDB\n(unified SQL)")
    medians.append(_median([t[2] for t in astral_series]))
    pc = ", ".join(
        (
            "failed"
            if t[2] is None
            else (
                f"compile~{t[0]:.1f} exec~{t[1]:.1f} total~{t[2]:.1f}ms"
                if t[0] is not None and t[1] is not None
                else f"wall_total~{t[2]:.1f}ms (no [time-sql] line on captured stderr)"
            )
        )
        for t in astral_series
    )
    print(f"AstralDB runs ({args.runs}x): median_total_ms={medians[-1]}  [{pc}]", flush=True)

    # DuckDB
    if not args.skip_duckdb:
        duck_times = time_duckdb(nc, no, nl, unified, args.runs)
        labels.append(f"DuckDB\n({nl:,} li)")
        medians.append(_median(duck_times))
        print(f"DuckDB runs ({args.runs}x): median wall ms={medians[-1]}  {duck_times}", flush=True)
    else:
        print("DuckDB skipped (--skip-duckdb).", flush=True)

    # MySQL
    if not args.skip_mysql:
        if importlib.util.find_spec("pymysql") is None and args.no_fetch:
            print("Skipping MySQL: pymysql not installed and --no-fetch set.", flush=True)
        else:
            if importlib.util.find_spec("pymysql") is None:
                ensure_import("pymysql", "pymysql", args.no_fetch)
            mysql_times = time_mysql(
                args.mysql_host,
                args.mysql_port,
                args.mysql_user,
                args.mysql_password,
                args.mysql_database,
                nc,
                no,
                nl,
                unified,
                args.runs,
            )
            labels.append(f"MySQL\n({nl:,} li)")
            medians.append(_median(mysql_times))
            print(f"MySQL runs ({args.runs}x): median wall ms={medians[-1]}  {mysql_times}", flush=True)
    else:
        print("MySQL skipped (--skip-mysql).", flush=True)

    # SQLite (stdlib; same batched DDL as AstralDB)
    if not args.skip_sqlite:
        parts = sqlite3.sqlite_version.split(".")
        maj = int(parts[0])
        mino = int(parts[1]) if len(parts) > 1 else 0
        if (maj, mino) < (3, 25):
            print(
                f"Skipping SQLite: need SQLite >= 3.25 for window functions (have {sqlite3.sqlite_version}).",
                flush=True,
            )
        else:
            sqlite_times = time_sqlite(nc, no, nl, unified, args.runs)
            labels.append(f"SQLite {sqlite3.sqlite_version}\n({nl:,} li)")
            medians.append(_median(sqlite_times))
            print(f"SQLite runs ({args.runs}x): median wall ms={medians[-1]}  {sqlite_times}", flush=True)
    else:
        print("SQLite skipped (--skip-sqlite).", flush=True)

    title = (
        "Unified SQL-92 analytic benchmark (CTE + JOIN + GROUP BY + window)\n"
        f"customers≈{nc:,} orders≈{no:,} line_items≈{nl:,} (scale={args.scale})"
    )
    plot_results(labels, medians, args.output.resolve(), title)

    ok = [lab.replace("\n", " ") for lab, ms in zip(labels, medians) if ms is not None]
    skipped = [lab.replace("\n", " ") for lab, ms in zip(labels, medians) if ms is None]
    print(f"Plotted engines ({len(ok)}): {', '.join(ok) if ok else '(none)'}", flush=True)
    if skipped:
        print(f"No bar (all runs failed or skipped): {', '.join(skipped)}", flush=True)

    print(
        "\nQuery: examples/benchmarks/benchmark_torture_unified.sql (same text on all engines).\n"
        f"AstralDB binary: {astral}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
