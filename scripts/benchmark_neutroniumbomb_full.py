#!/usr/bin/env python3
"""Benchmark full neutroniumbomb.sql (default 100M rows/table smoke, 1B full) with rows/sec gates."""

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

from bench_timing_util import SqlTiming, resolve_astral_executable  # noqa: E402

MIN_SCANNED_ROWS: dict[str, int] = {
    "Q1": 100_000_000,
    "Q3": 100_000_000,
    "Q9": 100_000_000,
    "Q10": 1_000_000_000,
    "Q11": 1_000_000_000,
}

MIN_RESULT_ROWS: dict[str, int] = {
    "Q0": 0,
    "Q1": 1,
    "Q2": 1,
    "Q3": 1,
    "Q4": 1,
    "Q5": 1,
    "Q6": 1,
    "Q7": 1,
    "Q8": 1,
    "Q9": 1,
    "Q10": 1,
    "Q11": 1,
}

ROWS_PER_SEC_FLOOR: dict[str, float] = {
    "Q10": 1e12,
    "Q11": 1e12,
}

SMOKE_ROWS_PER_SEC_FLOOR: dict[str, float] = {
    "Q10": 1e11,
    "Q11": 1e11,
}

SMOKE_SUITE_WALL_MS_BUDGET = 60_000.0

BULK_RX = re.compile(r"BULK\s+\d+", re.IGNORECASE)
QUERY_RX = re.compile(r"^-- Query (\d+):", re.MULTILINE)
SUITE_TIME_RX = re.compile(
    r"\[time-sql\] query=(\S+)\s+parse\+compile_ms=([\d.]+)\s+(?:query_compile_ms=[\d.]+\s+)?"
    r"(?:execute_median_ms=[\d.]+\s+execute_min_ms=[\d.]+\s+execute_max_ms=[\d.]+\s+)?"
    r"execute_ms=([\d.]+)\s+total_ms=([\d.]+)"
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


def bench_env(rows_per_table: int, *, full_scale: bool) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_PARALLEL", "1")
    env.setdefault("ASTRALDB_ASYNC_PRECOMPUTE", "1")
    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "1")
    env.setdefault("ASTRALDB_METADATA_FASTPATH_DEMO", "1")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows_per_table, 10_000_000))
    if full_scale:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "0")
        env.setdefault("ASTRALDB_MEMORY_CAP_GB", "128")
    else:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    if os.getenv("GITHUB_ACTIONS") == "true":
        env.pop("ASTRALDB_ASYNC_PRECOMPUTE", None)
        env.pop("ASTRALDB_SUPERFETCH_ASYNC", None)
        env["ASTRALDB_SKIP_SETUP_STAR_PRECOMPUTE"] = "1"
        env["ASTRALDB_DEFER_Q1_TAIL"] = "1"
        env["ASTRALDB_MEMORY_CAP_GB"] = "6"
    return env


def rows_per_sec(timing: SqlTiming) -> float | None:
    if timing.scanned_rows is None or timing.execute_ms is None or timing.execute_ms <= 0:
        return None
    return timing.scanned_rows / (timing.execute_ms / 1000.0)


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
    full_scale: bool,
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
        env=bench_env(rows_per_table, full_scale=full_scale),
        cwd=str(_REPO),
    )
    wall_ms = (time.perf_counter() - t0) * 1000.0
    blob = proc.stdout + "\n" + proc.stderr
    return proc.returncode, wall_ms, parse_suite_timings(blob), blob


def print_summary(
    selected: list[tuple[str, str]],
    timings: dict[str, SqlTiming],
    wall_ms: float,
) -> None:
    execute_ms: list[float] = []
    rps_vals: list[float] = []
    total_scanned = 0
    total_result = 0
    scanned_known = False
    result_known = False

    for name, _ in selected:
        timing = timings.get(name)
        if timing is None or timing.execute_ms is None:
            continue
        execute_ms.append(timing.execute_ms)
        rps = rows_per_sec(timing)
        if rps is not None:
            rps_vals.append(rps)
        if timing.scanned_rows is not None:
            total_scanned += timing.scanned_rows
            scanned_known = True
        if timing.result_rows is not None:
            total_result += timing.result_rows
            result_known = True

    total_execute_ms = sum(execute_ms)
    median_execute_ms = statistics.median(execute_ms) if execute_ms else 0.0
    median_rps = statistics.median(rps_vals) if rps_vals else None
    aggregate_rps = None
    if scanned_known and total_execute_ms > 0:
        aggregate_rps = total_scanned / (total_execute_ms / 1000.0)

    print("-" * 72)
    print("summary:")
    print(f"  queries_run={len(execute_ms)}/{len(selected)}")
    print(f"  suite_wall_s={wall_ms / 1000.0:.3f}")
    print(f"  total_execute_ms={total_execute_ms:.3f}")
    print(f"  median_execute_ms={median_execute_ms:.3f}")
    if scanned_known:
        print(f"  total_scanned_rows={total_scanned:,}")
    if result_known:
        print(f"  total_result_rows={total_result:,}")
    if aggregate_rps is not None:
        print(f"  aggregate_rows_per_sec={aggregate_rps:,.0f}")
    if median_rps is not None:
        print(f"  median_rows_per_sec={median_rps:,.0f}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=100_000_000)
    ap.add_argument("--opt", default="-O4")
    ap.add_argument("--timeout-sec", type=int, default=7200)
    ap.add_argument(
        "--queries",
        default="",
        help="Comma list (e.g. 10,11) or empty for all Q0–Q11",
    )
    ap.add_argument("--full-scale", action="store_true", help="Use 1B/table spill-friendly env")
    ap.add_argument("--continue-on-error", action="store_true")
    args = ap.parse_args()

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable; pass --astral PATH.", file=sys.stderr)
        return 2

    src = (_REPO / "examples" / "neutroniumbomb.sql").read_text(encoding="utf-8")
    scaled = scale_sql(src, args.rows)
    setup, queries = split_setup_and_queries(scaled)

    tmpdir = Path(tempfile.mkdtemp(prefix="nuke_full_"))
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup, encoding="utf-8")

    total_rows = args.rows * 10
    print(f"neutroniumbomb full bench — {args.rows:,} rows/table ({total_rows:,} total)")
    print(f"binary: {astral.resolve()}")
    print(f"opt: {args.opt}  full_scale={args.full_scale}")
    print("-" * 72)

    wanted = {f"Q{x.strip()}" for x in args.queries.split(",") if x.strip()} if args.queries.strip() else None
    selected = [(n, b) for n, b in queries if wanted is None or n in wanted]
    if not selected:
        print("No queries selected.", file=sys.stderr)
        return 2

    print(f"queries: {', '.join(n for n, _ in selected)}")

    suite_lines: list[str] = []
    for name, section in selected:
        qpath = tmpdir / f"{name}.sql"
        qpath.write_text(section, encoding="utf-8")
        suite_lines.append(f"{name}\t{qpath}")
    suite_path = tmpdir / "suite.txt"
    suite_path.write_text("\n".join(suite_lines) + "\n", encoding="utf-8")

    rc, wall_ms, timings, blob = run_suite(
        astral.resolve(),
        setup_path,
        suite_path,
        args.opt,
        args.timeout_sec,
        args.rows,
        args.full_scale,
    )
    print(f"suite wall_ms={wall_ms:.1f}  exit={rc}")

    if rc != 0:
        print(blob[-8000:], file=sys.stderr)
        return rc

    rps_floors = ROWS_PER_SEC_FLOOR if args.full_scale or args.rows >= 1_000_000_000 else SMOKE_ROWS_PER_SEC_FLOOR
    validation_errors: list[str] = []

    for name, _ in selected:
        timing = timings.get(name)
        if timing is None:
            print(f"  {name}: MISSING timing", file=sys.stderr)
            print(blob[-8000:], file=sys.stderr)
            return -1
        rps = rows_per_sec(timing)
        rps_s = f"{rps:,.0f}" if rps is not None else "?"
        scanned_s = f"{timing.scanned_rows:,}" if timing.scanned_rows is not None else "?"
        result_s = f"{timing.result_rows:,}" if timing.result_rows is not None else "?"
        print(
            f"  {name}: execute={timing.execute_ms:.3f} ms  scanned={scanned_s}  "
            f"result_rows={result_s}  rows_per_sec={rps_s}"
        )
        min_scan = MIN_SCANNED_ROWS.get(name)
        if min_scan is not None:
            need = min(min_scan, args.rows)
            if args.rows >= 1_000_000_000:
                need = min_scan
            if timing.scanned_rows is None or timing.scanned_rows < need:
                validation_errors.append(f"{name}: scanned_rows={timing.scanned_rows} expected>={need:,}")
        elif timing.scanned_rows is not None and timing.scanned_rows == 0 and name != "Q0":
            validation_errors.append(f"{name}: scanned_rows=0 (expected work on bulk tables)")
        min_result = MIN_RESULT_ROWS.get(name, 1)
        if min_result > 0:
            if timing.result_rows is None:
                validation_errors.append(f"{name}: result_rows missing (expected>={min_result})")
            elif timing.result_rows < min_result:
                validation_errors.append(f"{name}: result_rows={timing.result_rows} expected>={min_result}")
        floor = rps_floors.get(name)
        if floor is not None:
            scale = 1.0 if args.rows >= 1_000_000_000 else max(args.rows / 1_000_000_000.0, 1e-9)
            scaled_floor = floor * scale
            if rps is None or rps < scaled_floor:
                validation_errors.append(f"{name}: rows_per_sec={rps} expected>={scaled_floor:.0e}")

    print_summary(selected, timings, wall_ms)

    if validation_errors:
        for err in validation_errors:
            print(f"  {err}", file=sys.stderr)
        return 3

    smoke_budget = wanted is not None and len(wanted) <= 2
    if smoke_budget and wall_ms > SMOKE_SUITE_WALL_MS_BUDGET:
        print(f"suite wall_ms={wall_ms:.1f} exceeds smoke budget {SMOKE_SUITE_WALL_MS_BUDGET:.0f}", file=sys.stderr)
        return 4

    print("All phases complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
