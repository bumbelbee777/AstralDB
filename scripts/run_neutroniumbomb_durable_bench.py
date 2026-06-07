#!/usr/bin/env python3
"""Durable ACID benchmark: persistent DB, WAL fsync overlap, Q10/Q11 + interrupt/resume."""

from __future__ import annotations

import argparse
import json
import os
import re
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


def durable_env(rows: int, *, full_scale: bool) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_PARALLEL", "1")
    env.setdefault("ASTRALDB_ASYNC_PRECOMPUTE", "1")
    env.setdefault("ASTRALDB_SUPERFETCH_ASYNC", "1")
    env["ASTRALDB_WAL_FSYNC_ASYNC"] = "1"
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows, 10_000_000))
    if full_scale or rows >= 1_000_000_000:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "0")
        env.setdefault("ASTRALDB_MEMORY_CAP_GB", "128")
    else:
        env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    return env


def rows_per_sec(scanned: int | None, execute_ms: float | None) -> float | None:
    if scanned is None or execute_ms is None or execute_ms <= 0:
        return None
    return scanned / (execute_ms / 1000.0)


def _print_stats(label: str, rc: int, stats: dict) -> None:
    rps = stats.get("rows_per_sec")
    rps_s = f"{rps:.3e}" if rps else "?"
    print(
        f"  {label} exit={rc}  execute_ms={stats.get('execute_ms')}  "
        f"wal_quiesce_ms={stats.get('wal_quiesce_ms')}  scanned={stats.get('scanned_rows')}  "
        f"rows_per_sec={rps_s}"
    )


def run_timed_query(
    astral: Path,
    db_path: Path,
    setup_path: Path | None,
    query_path: Path,
    opt: str,
    env: dict[str, str],
    timeout: int,
    *,
    durable: bool,
    fresh_db: bool,
) -> tuple[int, str, dict]:
    if fresh_db:
        for suffix in ("", ".wal", ".query.ckpt"):
            p = Path(str(db_path) + suffix)
            if p.is_file():
                p.unlink()
    cmd = [str(astral), opt, "--database", str(db_path)]
    if setup_path is not None:
        cmd += ["--time-sql-setup", str(setup_path)]
    cmd += ["--time-sql", str(query_path)]
    if durable:
        cmd.append("--time-sql-durable")
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
        env=env,
        cwd=str(_REPO),
    )
    blob = proc.stdout + "\n" + proc.stderr
    timing = parse_time_sql_output(blob)
    stats = {
        "exit": proc.returncode,
        "execute_ms": timing.stable_ms(),
        "compile_ms": timing.compile_ms,
        "scanned_rows": timing.scanned_rows,
        "result_rows": timing.result_rows,
        "wal_quiesce_ms": timing.wal_quiesce_ms,
        "rows_per_sec": rows_per_sec(timing.scanned_rows, timing.stable_ms()),
    }
    return proc.returncode, blob, stats


def run_interrupt_demo(astral: Path, rows: int, opt: str, env: dict[str, str], timeout: int) -> dict:
    tmpdir = Path(tempfile.mkdtemp(prefix="nuke_durable_acid_"))
    db_path = tmpdir / "astraldb_session_acid_demo.db"
    ckpt_path = Path(str(db_path) + ".query.ckpt")
    setup = BULK_RX.sub(
        f"BULK {rows}",
        (_REPO / "examples/benchmarks/neutroniumbomb_demo_setup.sql").read_text(encoding="utf-8"),
    )
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup, encoding="utf-8")
    query_path = _REPO / "examples/benchmarks/neutroniumbomb_query.sql"
    demo_env = dict(env)
    demo_env["ASTRALDB_MEGAFUSION_DEMO_CKPT"] = "1"
    demo_env["ASTRALDB_MAX_BULK_ROWS"] = str(max(rows, 10_000))

    run1 = [
        str(astral),
        opt,
        "--database",
        str(db_path),
        "--time-sql-setup",
        str(setup_path),
        "--time-sql",
        str(query_path),
        "--time-sql-durable",
    ]
    proc1 = subprocess.run(run1, capture_output=True, text=True, env=demo_env, cwd=str(_REPO), timeout=timeout)
    if proc1.returncode != 0:
        return {"ok": False, "phase": "interrupt", "error": (proc1.stderr or proc1.stdout)[-1500:]}
    if not ckpt_path.is_file():
        return {"ok": False, "phase": "interrupt", "error": "missing query checkpoint"}

    demo_env.pop("ASTRALDB_MEGAFUSION_DEMO_CKPT", None)
    run2 = [
        str(astral),
        opt,
        "--database",
        str(db_path),
        "--time-sql",
        str(query_path),
        "--resume-checkpoint",
        str(ckpt_path),
        "--time-sql-durable",
    ]
    t0 = time.perf_counter()
    proc2 = subprocess.run(run2, capture_output=True, text=True, env=demo_env, cwd=str(_REPO), timeout=timeout)
    wall_ms = (time.perf_counter() - t0) * 1000.0
    timing = parse_time_sql_output(proc2.stdout + proc2.stderr)
    ok = (
        proc2.returncode == 0
        and wall_ms <= 1000.0
        and timing.scanned_rows is not None
        and timing.scanned_rows >= rows * 10
    )
    return {
        "ok": ok,
        "resume_wall_ms": wall_ms,
        "execute_ms": timing.stable_ms(),
        "wal_quiesce_ms": timing.wal_quiesce_ms,
        "scanned_rows": timing.scanned_rows,
        "rows_per_sec": rows_per_sec(timing.scanned_rows, timing.stable_ms()),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--astral", type=Path, default=None)
    ap.add_argument("--rows", type=int, default=100_000_000)
    ap.add_argument("--opt", default="-O4")
    ap.add_argument("--timeout", type=int, default=7200)
    ap.add_argument("--full-scale", action="store_true")
    ap.add_argument("--summary", type=Path, default=_REPO / "media" / "neutroniumbomb_durable_bench.json")
    ap.add_argument("--skip-interrupt", action="store_true")
    args = ap.parse_args()

    astral = args.astral if args.astral is not None else resolve_astral_executable(_REPO)
    if astral is None or not astral.is_file():
        print("No astraldb executable.", file=sys.stderr)
        return 2

    full_scale = args.full_scale or args.rows >= 1_000_000_000
    env = durable_env(args.rows, full_scale=full_scale)
    tmpdir = Path(tempfile.mkdtemp(prefix="nuke_durable_"))
    db_q10 = tmpdir / "astraldb_session_durable_q10.db"
    db_q11 = tmpdir / "astraldb_session_durable_q11.db"
    setup_src = BULK_RX.sub(
        f"BULK {args.rows}",
        (_REPO / "examples/benchmarks/neutroniumbomb_setup.sql").read_text(encoding="utf-8"),
    )
    setup_path = tmpdir / "setup.sql"
    setup_path.write_text(setup_src, encoding="utf-8")
    q10_path = _REPO / "examples/benchmarks/neutroniumbomb_query.sql"
    q11_path = _REPO / "examples/benchmarks/neutroniumbomb_htap.sql"

    print(f"neutroniumbomb durable ACID bench — {args.rows:,} rows/table")
    print(f"binary: {astral.resolve()}  WAL async fsync=1  persistent DB")
    print("-" * 72)

    results: dict = {"rows_per_table": args.rows, "full_scale": full_scale, "queries": {}, "acid_demo": None}

    print("Q10 (durable WAL + fsync overlap)…", flush=True)
    rc, blob, stats = run_timed_query(
        astral.resolve(), db_q10, setup_path, q10_path, args.opt, env, args.timeout,
        durable=True, fresh_db=True,
    )
    results["queries"]["Q10"] = stats
    _print_stats("Q10", rc, stats)
    if rc != 0:
        print(blob[-3000:], file=sys.stderr)
        return rc

    print("reopen Q10 (WAL durability check)…", flush=True)
    rc, blob, reopen = run_timed_query(
        astral.resolve(), db_q10, None, q10_path, args.opt, env, args.timeout,
        durable=True, fresh_db=False,
    )
    results["reopen_q10"] = reopen
    _print_stats("reopen Q10", rc, reopen)
    if rc != 0 or reopen.get("scanned_rows") is None or reopen["scanned_rows"] < args.rows * 10:
        print(blob[-3000:], file=sys.stderr)
        return 5

    print("Q11 HTAP (durable COMMIT + megafusion)…", flush=True)
    rc, blob, stats = run_timed_query(
        astral.resolve(), db_q11, setup_path, q11_path, args.opt, env, args.timeout,
        durable=True, fresh_db=True,
    )
    results["queries"]["Q11"] = stats
    _print_stats("Q11", rc, stats)
    if rc != 0:
        print(blob[-3000:], file=sys.stderr)
        return rc

    if not args.skip_interrupt:
        print("ACID interrupt/resume (durable WAL)…", flush=True)
        acid = run_interrupt_demo(astral.resolve(), min(args.rows, 50_000), args.opt, env, min(args.timeout, 300))
        results["acid_demo"] = acid
        print(
            f"  ok={acid.get('ok')}  resume_wall_ms={acid.get('resume_wall_ms')}  "
            f"execute_ms={acid.get('execute_ms')}  wal_quiesce_ms={acid.get('wal_quiesce_ms')}"
        )
        if not acid.get("ok"):
            print(acid.get("error", "interrupt demo failed"), file=sys.stderr)
            return 3

    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"summary: {args.summary}")
    print("Durable ACID benchmark complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
