#!/usr/bin/env python3
"""Profile neutroniumbomb Q10 megafusion with region timings and derived rows/sec."""

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


def bench_env(rows_per_table: int, *, full_scale: bool) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_PARALLEL", "1")
    env.setdefault("ASTRALDB_ASYNC_PRECOMPUTE", "1")
    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "1")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows_per_table, 10_000_000))
    if full_scale or rows_per_table >= 1_000_000_000:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "0")
        env.setdefault("ASTRALDB_MEMORY_CAP_GB", "128")
    else:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    return env


def run_q10(
    astral: Path,
    setup_path: Path,
    query_path: Path,
    opt: str,
    timeout: int,
    rows: int,
    full_scale: bool,
    profile_path: Path | None,
    query_name: str = "Q10",
) -> tuple[int, str, dict]:
    env = bench_env(rows, full_scale=full_scale)
    if profile_path is not None:
        profile_path.parent.mkdir(parents=True, exist_ok=True)
        env["ASTRALDB_PROFILE_OUTPUT"] = str(profile_path)
        env["ASTRALDB_PROFILE_NAME"] = query_name
    cmd = [str(astral), "-m", opt, "--time-sql-setup", str(setup_path), "--time-sql", str(query_path)]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
            env=env,
            cwd=str(_REPO),
        )
    except subprocess.TimeoutExpired as ex:
        blob = (ex.stdout or "") + "\n" + (ex.stderr or "") + f"\n[timeout after {timeout}s]\n"
        timing = parse_time_sql_output(blob)
        return 124, blob, {
            "exit": 124,
            "execute_ms": timing.stable_ms(),
            "scanned_rows": timing.scanned_rows,
            "result_rows": timing.result_rows,
            "timeout": True,
        }
    blob = proc.stdout + "\n" + proc.stderr
    timing = parse_time_sql_output(blob)
    stats: dict = {
        "exit": proc.returncode,
        "execute_ms": timing.stable_ms(),
        "scanned_rows": timing.scanned_rows,
        "result_rows": timing.result_rows,
    }
    if timing.scanned_rows and timing.execute_ms and timing.execute_ms > 0:
        stats["rows_per_sec"] = timing.scanned_rows / (timing.execute_ms / 1000.0)
    if profile_path is not None and profile_path.is_file():
        try:
            stats["profile"] = json.loads(profile_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            stats["profile"] = None
    return proc.returncode, blob, stats


def print_regions(stats: dict) -> None:
    prof = stats.get("profile")
    if not isinstance(prof, list) or not prof or not isinstance(prof[0], dict):
        return
    regions = prof[0].get("region_timings")
    if not isinstance(regions, dict) or not regions:
        return
    ranked = sorted(((k, float(v) / 1e6) for k, v in regions.items()), key=lambda x: -x[1])
    total = sum(ms for _, ms in ranked)
    print(f"  region_timings_ms (sum={total:.3f}):")
    for name, ms in ranked[:12]:
        print(f"    {ms:8.3f}  {name}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=100_000_000)
    ap.add_argument("--opt", default="-O4")
    ap.add_argument("--timeout", type=int, default=7200)
    ap.add_argument("--full-scale", action="store_true")
    ap.add_argument("--profile-dir", type=Path, default=None)
    ap.add_argument("--summary", type=Path, default=None)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--repeat-gap-ms", type=int, default=200)
    args = ap.parse_args()

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable; pass --astral PATH.", file=sys.stderr)
        return 2

    setup_src = BULK_RX.sub(
        f"BULK {args.rows}",
        (_REPO / "examples/benchmarks/neutroniumbomb_setup.sql").read_text(encoding="utf-8"),
    )
    query_path = _REPO / "examples/benchmarks/neutroniumbomb_query.sql"
    tmpdir = Path(tempfile.mkdtemp(prefix="nuke_prof_"))
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup_src, encoding="utf-8")

    profile_dir = args.profile_dir
    if profile_dir is None and args.summary is not None:
        profile_dir = args.summary.parent / "profiles"

    full_scale = args.full_scale or args.rows >= 1_000_000_000
    print(f"neutroniumbomb Q10 profile — {args.rows:,} rows/table  opt={args.opt}  full_scale={full_scale}")
    print(f"binary: {astral.resolve()}")
    print("-" * 72)

    gap_s = max(0, args.repeat_gap_ms) / 1000.0
    samples: list[float] = []
    rps_samples: list[float] = []
    stats: dict = {}
    rc = 0
    blob = ""

    for w in range(max(0, args.warmup)):
        if w > 0 and gap_s > 0:
            time.sleep(gap_s)
        rc, blob, stats = run_q10(
            astral.resolve(),
            setup_path,
            query_path,
            args.opt,
            args.timeout,
            args.rows,
            full_scale,
            None,
        )
        if rc != 0:
            break

    for r in range(max(1, args.repeat)):
        if r > 0 or args.warmup > 0:
            if gap_s > 0:
                time.sleep(gap_s)
        prof_path = None
        if profile_dir is not None and r == args.repeat - 1:
            prof_path = profile_dir / f"Q10_rows_{args.rows}.json"
        rc, blob, stats = run_q10(
            astral.resolve(),
            setup_path,
            query_path,
            args.opt,
            args.timeout,
            args.rows,
            full_scale,
            prof_path,
        )
        if rc != 0:
            break
        ex = stats.get("execute_ms")
        if ex is not None:
            samples.append(float(ex))
        rps = stats.get("rows_per_sec")
        if rps is not None:
            rps_samples.append(float(rps))

    if samples:
        stats = dict(stats)
        stats["execute_ms"] = statistics.median(samples)
        stats["execute_ms_min"] = min(samples)
        stats["execute_ms_max"] = max(samples)
        stats["execute_ms_samples"] = samples
    if rps_samples:
        stats["rows_per_sec"] = statistics.median(rps_samples)
        stats["rows_per_sec_min"] = min(rps_samples)
        stats["rows_per_sec_max"] = max(rps_samples)

    print(
        f"  exit={rc}  execute_ms={stats.get('execute_ms')}  scanned={stats.get('scanned_rows')}  "
        f"rows_per_sec={stats.get('rows_per_sec', 0):.3e}"
    )
    print_regions(stats)
    if rc != 0:
        print(blob[-4000:], file=sys.stderr)
        return rc

    if args.summary is not None:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "query": "Q10",
            "rows_per_table": args.rows,
            "full_scale": full_scale,
            "opt": args.opt,
            "stats": stats,
        }
        args.summary.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"summary: {args.summary}")
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
