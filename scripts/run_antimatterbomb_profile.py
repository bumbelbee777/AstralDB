#!/usr/bin/env python3
"""Run antimatterbomb.sql query sections with --time-sql and optional region profiling."""

from __future__ import annotations

import argparse
import json
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

from bench_timing_util import parse_time_sql_output, resolve_astral_executable  # noqa: E402

BULK_RX = re.compile(r"BULK\s+\d+", re.IGNORECASE)
QUERY_RX = re.compile(r"^-- Query (\d+):", re.MULTILINE)
TIME_SQL_RX = re.compile(r"\[time-sql\].*")
PROFILE_RX = re.compile(r"\[time-sql-profile\].*")


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
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_COLUMNAR_COMMIT", "1")
    # Parallel zip/LUT matches production; set ASTRALDB_SEMISTRUCTURED_PARALLEL=0 for deterministic single-thread profiles.
    env.setdefault("ASTRALDB_SEMISTRUCTURED_PARALLEL", "1")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows_per_table, 10_000_000))
    return env


def run_timed(
    astral: Path,
    setup_path: Path,
    query_path: Path,
    opt: str,
    timeout: int,
    profile_dir: Path | None,
    query_name: str,
    rows_per_table: int,
) -> tuple[int, str, dict]:
    env = bench_env(rows_per_table)
    if profile_dir is not None:
        profile_dir.mkdir(parents=True, exist_ok=True)
        env["ASTRALDB_PROFILE_OUTPUT"] = str(profile_dir / f"{query_name}.json")
        env["ASTRALDB_PROFILE_NAME"] = query_name
    cmd = [str(astral), "-m", opt, "--time-sql-setup", str(setup_path), "--time-sql", str(query_path)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False, env=env)
    except subprocess.TimeoutExpired as Ex:
        blob = (Ex.stdout or "") + "\n" + (Ex.stderr or "") + f"\n[timeout after {timeout}s]\n"
        timing = parse_time_sql_output(blob)
        return 124, blob, {
            "exit": 124,
            "execute_ms": timing.stable_ms(),
            "scanned_rows": timing.scanned_rows,
            "result_rows": timing.result_rows,
            "fast_path_flags": None,
            "timeout": True,
        }
    blob = proc.stdout + "\n" + proc.stderr
    timing = parse_time_sql_output(blob)
    stats = {
        "exit": proc.returncode,
        "execute_ms": timing.stable_ms(),
        "scanned_rows": timing.scanned_rows,
        "result_rows": timing.result_rows,
        "fast_path_flags": None,
    }
    for line in blob.splitlines():
        if "fast_path_flags=" in line:
            m = re.search(r"fast_path_flags=(\d+)", line)
            if m:
                stats["fast_path_flags"] = int(m.group(1))
    if profile_dir is not None:
        prof_path = profile_dir / f"{query_name}.json"
        if prof_path.is_file():
            try:
                stats["profile"] = json.loads(prof_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                stats["profile"] = None
    return proc.returncode, blob, stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=200_000)
    ap.add_argument("--opt", default="-O4")
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument(
        "--queries",
        default="1,2,3,4,5,6,7,8,9,10",
        help="Comma list e.g. 1,3,9 or omit flag value for all (use --queries '' on shell)",
    )
    ap.add_argument(
        "--slow-ms",
        type=float,
        default=100.0,
        help="Flag queries with median execute_ms above this threshold (default 100)",
    )
    ap.add_argument("--profile-dir", type=Path, default=None, help="Write per-query profile JSON here")
    ap.add_argument("--summary", type=Path, default=None, help="Write combined summary JSON")
    ap.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Run remaining queries after a failure",
    )
    ap.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="Untimed query runs before each measured run (default 1)",
    )
    ap.add_argument(
        "--repeat",
        type=int,
        default=3,
        help="Timed repetitions; report median execute_ms (default 3)",
    )
    ap.add_argument(
        "--repeat-gap-ms",
        type=int,
        default=500,
        help="Sleep between timed repetitions (default 500)",
    )
    ap.add_argument(
        "--assert-max-ms",
        type=float,
        default=None,
        help="Exit 1 if any query median execute_ms exceeds this threshold",
    )
    ap.add_argument(
        "--assert-bulk-max-s",
        type=float,
        default=None,
        help="Exit 1 if setup bulk region bulk_pass_bits_* exceeds this many seconds (from profile JSON)",
    )
    args = ap.parse_args()

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable; pass --astral PATH.", file=sys.stderr)
        return 2

    src = (_REPO / "examples" / "antimatterbomb.sql").read_text(encoding="utf-8")
    scaled = scale_sql(src, args.rows)
    setup, queries = split_setup_and_queries(scaled)
    if not queries:
        print("No -- Query N: sections in antimatterbomb.sql", file=sys.stderr)
        return 2

    wanted = {f"Q{x.strip()}" for x in args.queries.split(",") if x.strip()} if args.queries else None
    tmpdir = Path(tempfile.mkdtemp(prefix="amb_prof_"))
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup, encoding="utf-8")

    profile_dir = args.profile_dir
    if profile_dir is None and args.summary is not None:
        profile_dir = tmpdir / "profiles"

    print(f"antimatterbomb profile — {args.rows:,} rows/table  opt={args.opt}")
    print(f"binary: {astral.resolve()}")
    print("-" * 72)

    results: dict[str, dict] = {}
    for name, section in queries:
        if wanted is not None and name not in wanted:
            continue
        qpath = tmpdir / f"{name}.sql"
        qpath.write_text(section, encoding="utf-8")
        print(f"{name}…", flush=True)
        warmup = max(0, args.warmup)
        repeat = max(1, args.repeat)
        gap_s = max(0, args.repeat_gap_ms) / 1000.0
        rc = 0
        blob = ""
        stats: dict = {}
        samples: list[float] = []
        for w in range(warmup):
            if w > 0 and gap_s > 0:
                time.sleep(gap_s)
            rc, blob, stats = run_timed(
                astral.resolve(),
                setup_path,
                qpath,
                args.opt,
                args.timeout,
                None if w < warmup - 1 or repeat > 1 else profile_dir,
                name,
                args.rows,
            )
            if rc != 0:
                break
        for r in range(repeat):
            if r > 0 or warmup > 0:
                if gap_s > 0:
                    time.sleep(gap_s)
            rc, blob, stats = run_timed(
                astral.resolve(),
                setup_path,
                qpath,
                args.opt,
                args.timeout,
                profile_dir if r == repeat - 1 else None,
                name,
                args.rows,
            )
            if rc != 0:
                break
            ex = stats.get("execute_ms")
            if ex is not None:
                samples.append(float(ex))
        if samples:
            stats = dict(stats)
            stats["execute_ms"] = statistics.median(samples)
            stats["execute_ms_min"] = min(samples)
            stats["execute_ms_max"] = max(samples)
            stats["execute_ms_samples"] = samples
        results[name] = stats
        fp = stats.get("fast_path_flags")
        ex = stats.get("execute_ms")
        scan = stats.get("scanned_rows")
        res = stats.get("result_rows")
        ex_min = stats.get("execute_ms_min")
        ex_max = stats.get("execute_ms_max")
        if ex_min is not None and ex_max is not None and ex_min != ex_max:
            print(
                f"  exit={rc}  execute_ms={ex} (median of {len(samples)})"
                f"  min={ex_min}  max={ex_max}  scanned={scan}  result={res}  fast_path_flags={fp}"
            )
        else:
            print(f"  exit={rc}  execute_ms={ex}  scanned={scan}  result={res}  fast_path_flags={fp}")
        if rc != 0:
            print(blob[-4000:], file=sys.stderr)
            if not args.continue_on_error:
                return rc
        prof = stats.get("profile")
        if isinstance(prof, list) and prof and isinstance(prof[0], dict):
            regions = prof[0].get("region_timings")
            if isinstance(regions, dict) and regions:
                ranked = sorted(
                    ((k, float(v) / 1e6) for k, v in regions.items()),
                    key=lambda x: -x[1],
                )
                total = sum(ms for _, ms in ranked)
                print(f"  region_timings_ms (sum={total:.3f}, nested scopes may double-count):")
                for name, ms in ranked:
                    print(f"    {ms:8.3f}  {name}")

    print("-" * 72)
    ranked = sorted(
        ((n, float(s.get("execute_ms") or 0.0)) for n, s in results.items()),
        key=lambda x: -x[1],
    )
    if ranked:
        print("query ranking (median execute_ms):")
        for name, ms in ranked:
            tag = " SLOW" if ms > args.slow_ms else ""
            print(f"  {ms:9.1f} ms  {name}{tag}")
    slow = [(n, ms) for n, ms in ranked if ms > args.slow_ms]
    if slow:
        print(f"above {args.slow_ms:g} ms: {', '.join(f'{n}({ms:.1f})' for n, ms in slow)}")
    if args.summary is not None:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "rows_per_table": args.rows,
            "opt": args.opt,
            "slow_ms": args.slow_ms,
            "queries": results,
            "ranking": [{"query": n, "execute_ms": ms} for n, ms in ranked],
            "slow_queries": [{"query": n, "execute_ms": ms} for n, ms in slow],
        }
        args.summary.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"summary: {args.summary}")
    if args.assert_max_ms is not None:
        for name, stats in results.items():
            ex = stats.get("execute_ms")
            if ex is not None and float(ex) > args.assert_max_ms:
                print(
                    f"ASSERT FAIL: {name} execute_ms={ex} > {args.assert_max_ms}",
                    file=sys.stderr,
                )
                return 1
    if args.assert_bulk_max_s is not None:
        bulk_s = 0.0
        for stats in results.values():
            prof = stats.get("profile")
            if not isinstance(prof, list) or not prof or not isinstance(prof[0], dict):
                continue
            regions = prof[0].get("region_timings")
            if not isinstance(regions, dict):
                continue
            for key, ns in regions.items():
                if key.startswith("bulk_pass_bits"):
                    bulk_s = max(bulk_s, float(ns) / 1e9)
        if bulk_s > args.assert_bulk_max_s:
            print(
                f"ASSERT FAIL: bulk_pass_bits {bulk_s:.3f}s > {args.assert_bulk_max_s}s",
                file=sys.stderr,
            )
            return 1
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
