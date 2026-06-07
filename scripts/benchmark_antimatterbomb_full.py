#!/usr/bin/env python3
"""Benchmark full antimatterbomb.sql (default 100M rows/table) with per-phase timings."""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
if str(_REPO / "scripts") not in sys.path:
    sys.path.insert(0, str(_REPO / "scripts"))

from bench_timing_util import SqlTiming, parse_time_sql_output, resolve_astral_executable  # noqa: E402

# Minimum scanned_rows (lazy bulk filter/join must touch base table scale).
MIN_SCANNED_ROWS: dict[str, int] = {
    "Q1": 100_000_000,
    "Q2": 100_000_000,
    "Q3": 100_000_000,
    "Q4": 100_000_000,
    "Q6": 100_000_000,
    "Q7": 100_000_000,
    "Q8": 100_000_000,
    "Q9": 100_000_000,
    "Q10": 100_000_000,
}

# Queries with LIMIT must return at least one row when predicates match synthetic data.
MIN_RESULT_ROWS: dict[str, int] = {
    "Q1": 1,
    "Q3": 1,
    "Q4": 1,
    "Q5": 11,
    "Q6": 1,
    "Q7": 1,
    "Q8": 1,
    "Q9": 1,
    "Q10": 1,
}

SUITE_WALL_MS_BUDGET = 30_000.0

BULK_RX = re.compile(r"BULK\s+\d+", re.IGNORECASE)
QUERY_RX = re.compile(r"^-- Query (\d+):", re.MULTILINE)
SUITE_TIME_RX = re.compile(
    r"\[time-sql\] query=(\S+)\s+parse\+compile_ms=([\d.]+)\s+execute_ms=([\d.]+)\s+total_ms=([\d.]+)"
    r"(?:\s+scanned_rows=(\d+))?(?:\s+result_rows=(\d+))?"
)


def scale_sql(text: str, rows: int) -> str:
    return BULK_RX.sub(f"BULK {rows}", text)


def split_setup_and_queries(text: str) -> tuple[str, list[tuple[str, str]]]:
    parts = QUERY_RX.split(text)
    if len(parts) < 2:
        return text, []
    setup = parts[0]
    queries: list[tuple[str, str]] = []
    for i in range(1, len(parts), 2):
        qn = parts[i]
        body = parts[i + 1] if i + 1 < len(parts) else ""
        queries.append((f"Q{qn}", f"-- Query {qn}:" + body))
    return setup, queries


def bench_env(rows_per_table: int) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "0")
    env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows_per_table, 10_000_000))
    return env


def parse_suite_timings(blob: str) -> dict[str, SqlTiming]:
    out: dict[str, SqlTiming] = {}
    for m in SUITE_TIME_RX.finditer(blob):
        name = m.group(1)
        compile_ms = float(m.group(2))
        execute_ms = float(m.group(3))
        total_ms = float(m.group(4))
        scanned = int(m.group(5)) if m.group(5) else None
        result = int(m.group(6)) if m.group(6) else None
        out[name] = SqlTiming(compile_ms, execute_ms, execute_ms, execute_ms, total_ms, scanned, result)
    return out


def run_suite(
    astral: Path,
    setup_path: Path,
    suite_path: Path,
    opt: str,
    timeout_s: float,
    rows_per_table: int,
) -> tuple[int, float, dict[str, SqlTiming], str]:
    cmd = [
        str(astral),
        "-m",
        opt,
        "--time-sql-setup",
        str(setup_path),
        "--time-sql-suite",
        str(suite_path),
    ]
    t0 = time.perf_counter()
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout_s,
        check=False,
        env=bench_env(rows_per_table),
    )
    wall_ms = (time.perf_counter() - t0) * 1000.0
    blob = proc.stdout + "\n" + proc.stderr
    return proc.returncode, wall_ms, parse_suite_timings(blob), blob


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=100_000_000)
    ap.add_argument("--opt", default="-O4")
    ap.add_argument("--timeout-sec", type=int, default=7200)
    ap.add_argument("--warmup", type=int, default=0)
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--queries", default="", help="Comma list e.g. 1,3,10 or empty for all")
    ap.add_argument("--setup-only", action="store_true")
    ap.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Run all queries even if one fails (report errors at end).",
    )
    args = ap.parse_args()

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable; pass --astral PATH.", file=sys.stderr)
        return 2

    src = (_REPO / "examples" / "antimatterbomb.sql").read_text(encoding="utf-8")
    scaled = scale_sql(src, args.rows)
    setup, queries = split_setup_and_queries(scaled)

    tmpdir = Path(tempfile.mkdtemp(prefix="amb_full_"))
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup, encoding="utf-8")

    total_rows = args.rows * 5
    print(f"antimatterbomb full bench — {args.rows:,} rows/table ({total_rows:,} total)")
    print(f"binary: {astral.resolve()}")
    print(f"opt: {args.opt}  timeout: {args.timeout_sec}s  runs: {args.runs}")
    print("-" * 72)

    wanted = {f"Q{x.strip()}" for x in args.queries.split(",") if x.strip()} if args.queries else None
    selected = [(n, b) for n, b in queries if wanted is None or n in wanted]

    if args.setup_only:
        cmd = [str(astral), "-m", args.opt, "-s", str(setup_path)]
        t0 = time.perf_counter()
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=args.timeout_sec,
            check=False,
            env=bench_env(args.rows),
        )
        setup_ms = (time.perf_counter() - t0) * 1000.0
        print(f"setup (DDL + 5x BULK): exit={proc.returncode}  wall_ms={setup_ms:.1f}")
        return proc.returncode

    if not selected:
        print("No queries selected.", file=sys.stderr)
        return 2

    suite_lines: list[str] = []
    for name, section in selected:
        qpath = tmpdir / f"{name}.sql"
        qpath.write_text(section, encoding="utf-8")
        suite_lines.append(f"{name}\t{qpath}")
    suite_path = tmpdir / "suite.txt"
    suite_path.write_text("\n".join(suite_lines) + "\n", encoding="utf-8")

    print("running setup + queries in one session (no per-query setup replay)…", flush=True)
    rc, wall_ms, timings, blob = run_suite(
        astral.resolve(),
        setup_path,
        suite_path,
        args.opt,
        args.timeout_sec,
        args.rows,
    )
    print(f"suite wall_ms={wall_ms:.1f}  exit={rc}")

    results: list[tuple[str, float | None, int, SqlTiming]] = []
    failures: list[tuple[str, int, str]] = []
    validation_errors: list[str] = []
    scale = args.rows

    for name, _ in selected:
        timing = timings.get(name)
        if timing is None:
            failures.append((name, -1, f"no [time-sql] line for {name}\n{blob[-4000:]}"))
            print(f"  {name}: MISSING timing", file=sys.stderr)
            if not args.continue_on_error:
                return -1
            continue
        ms = timing.stable_ms()
        qrc = 0 if not timing.execute_ms else 0
        if rc != 0 and name == selected[-1][0]:
            qrc = rc
        scan_s = timing.scanned_rows if timing.scanned_rows is not None else "?"
        result_s = timing.result_rows if timing.result_rows is not None else "?"
        print(f"  {name}: execute={ms:.1f} ms  scanned={scan_s}  result={result_s}")
        results.append((name, ms, qrc, timing))
        min_scan = MIN_SCANNED_ROWS.get(name)
        if min_scan is not None:
            need = min_scan if min_scan <= scale else scale
            if timing.scanned_rows is None or timing.scanned_rows < need:
                validation_errors.append(f"{name}: scanned_rows={timing.scanned_rows} expected>={need:,}")
        min_result = MIN_RESULT_ROWS.get(name)
        if min_result is not None and timing.result_rows is not None and timing.result_rows < min_result:
            validation_errors.append(f"{name}: result_rows={timing.result_rows} expected>={min_result}")

    if rc != 0 and not failures:
        failures.append(("suite", rc, blob[-4000:]))
        print(blob[-4000:], file=sys.stderr)

    print("-" * 72)
    ok = [ms for _, ms, _, _ in results if ms is not None]
    if ok:
        print(f"query phases: {len(results)}  median={statistics.median(ok):.1f} ms  max={max(ok):.1f} ms")
    ranked = sorted(((n, ms if ms is not None else -1.0) for n, ms, _, _ in results), key=lambda x: -x[1])
    if ranked:
        print("ranking:")
        for name, ms in ranked:
            slow = " SLOW" if ms > 100.0 else ""
            print(f"  {ms:9.1f} ms  {name}{slow}")
    if failures:
        print(f"failed: {', '.join(n for n, _, _ in failures)}", file=sys.stderr)
        return failures[0][1] if failures[0][1] >= 0 else rc
    if validation_errors:
        print("validation failed:", file=sys.stderr)
        for err in validation_errors:
            print(f"  {err}", file=sys.stderr)
        return 3
    if wall_ms > SUITE_WALL_MS_BUDGET:
        print(
            f"validation failed: suite wall_ms={wall_ms:.1f} exceeds budget {SUITE_WALL_MS_BUDGET:.0f} ms",
            file=sys.stderr,
        )
        return 4
    print("All phases complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
