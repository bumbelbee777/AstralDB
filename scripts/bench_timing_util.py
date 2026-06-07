"""Shared helpers for AstralDB benchmark timing (--time-sql output parsing)."""

from __future__ import annotations

import os
import platform
import re
import statistics
import subprocess
import tempfile
import time
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Optional


def resolve_astral_executable(repo_root: Path) -> Optional[Path]:
    bin_dir = repo_root / "bin"
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

TIME_RX = re.compile(
    r"parse\+compile_ms=([\d.]+)\s+"
    r"(?:query_compile_ms=[\d.]+\s+)?"
    r"(?:execute_median_ms=([\d.]+)\s+execute_min_ms=([\d.]+)\s+execute_max_ms=([\d.]+)\s+)?"
    r"execute_ms=([\d.]+)\s+total_ms=([\d.]+)"
    r"(?:\s+runs=(\d+))?"
    r"(?:\s+scanned_rows=(\d+))?"
    r"(?:\s+result_rows=(\d+))?"
    r"(?:\s+fast_path_flags=(\d+))?"
    r"(?:\s+min_scanned_expected=(\d+))?"
    r"(?:\s+integrity_fail=1(?:\s+msg=([^\n]+))?)?"
    r"(?:\s+wal_quiesce_ms=([\d.]+))?"
    r"(?:\s+durable=1)?"
)


@dataclass(frozen=True)
class SqlTiming:
    compile_ms: Optional[float]
    execute_ms: Optional[float]
    execute_min_ms: Optional[float]
    execute_max_ms: Optional[float]
    total_ms: Optional[float]
    scanned_rows: Optional[int] = None
    result_rows: Optional[int] = None
    wal_quiesce_ms: Optional[float] = None
    fast_path_flags: Optional[int] = None
    min_scanned_expected: Optional[int] = None
    integrity_fail: bool = False
    integrity_msg: Optional[str] = None

    def stable_ms(self) -> Optional[float]:
        if self.execute_ms is not None:
            return self.execute_ms
        return self.total_ms

    def durable_ms(self) -> Optional[float]:
        if self.execute_ms is None:
            return None
        wq = self.wal_quiesce_ms or 0.0
        return self.execute_ms + wq


def astral_metadata_env(max_bulk_rows: int = 10_000_000) -> dict[str, str]:
    """Universal lazy-bulk metadata manifests + durable bench knobs (no async superfetch demo)."""
    env = os.environ.copy()
    env.setdefault("ASTRALDB_SHARD_COMPRESS", "lz4")
    env.setdefault("ASTRALDB_WAL_FSYNC_ASYNC", "1")
    env.setdefault("ASTRALDB_DISABLE_BULK_SPILL", "1")
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_METADATA_FASTPATH_DEMO", "1")
    env.setdefault("ASTRALDB_DISABLE_METADATA_INSERT", "0")
    env["ASTRALDB_MAX_BULK_ROWS"] = str(max(10_000, int(max_bulk_rows)))
    env.setdefault("ASTRALDB_MEMORY_CAP_GB", "8")
    return env


@dataclass(frozen=True)
class FastPathValidation:
    ok: bool
    message: str


def validate_fast_path_output(
    timing: SqlTiming,
    *,
    require_fast_path: bool = False,
    min_result_rows: int = 0,
    max_execute_ms: Optional[float] = 1.0,
) -> FastPathValidation:
    if timing.integrity_fail:
        msg = timing.integrity_msg or "integrity_fail"
        return FastPathValidation(False, f"integrity_fail: {msg}")
    if require_fast_path and not timing.fast_path_flags:
        return FastPathValidation(False, "fast_path_flags=0 (metadata path did not engage)")
    if min_result_rows > 0 and (timing.result_rows is None or timing.result_rows < min_result_rows):
        got = timing.result_rows if timing.result_rows is not None else "?"
        return FastPathValidation(False, f"result_rows={got} < required {min_result_rows}")
    if max_execute_ms is not None and timing.durable_ms() is not None:
        if timing.durable_ms() > max_execute_ms:
            return FastPathValidation(
                False,
                f"durable_ms={timing.durable_ms():.3f} > {max_execute_ms} ms cap",
            )
    return FastPathValidation(True, "ok")


def parse_time_sql_output(blob: str) -> SqlTiming:
    anchor = blob.find("[time-sql]")
    if anchor >= 0:
        blob = blob[anchor:]
    m = TIME_RX.search(blob)
    if not m:
        integrity = "integrity_fail=1" in blob
        im = re.search(r"integrity_fail=1(?:\s+msg=([^\n]+))?", blob)
        return SqlTiming(
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            integrity,
            im.group(1).strip() if im and im.group(1) else None,
        )
    compile_ms = float(m.group(1))
    median_ms = float(m.group(2)) if m.group(2) else None
    exec_min = float(m.group(3)) if m.group(3) else None
    exec_max = float(m.group(4)) if m.group(4) else None
    execute_ms = float(m.group(5))
    total_ms = float(m.group(6))
    scanned_rows = int(m.group(8)) if m.group(8) else None
    result_rows = int(m.group(9)) if m.group(9) else None
    fast_path_flags = int(m.group(10)) if m.group(10) else None
    min_scanned_expected = int(m.group(11)) if m.group(11) else None
    integrity_fail = m.group(12) is not None or "integrity_fail=1" in blob
    integrity_msg = m.group(12).strip() if m.group(12) else None
    wal_quiesce_ms = float(m.group(13)) if m.group(13) else None
    if wal_quiesce_ms is None:
        wq = re.search(r"wal_quiesce_ms=([\d.]+)", blob)
        if wq:
            wal_quiesce_ms = float(wq.group(1))
    if median_ms is not None:
        execute_ms = median_ms
    return SqlTiming(
        compile_ms,
        execute_ms,
        exec_min,
        exec_max,
        total_ms,
        scanned_rows,
        result_rows,
        wal_quiesce_ms,
        fast_path_flags,
        min_scanned_expected,
        integrity_fail,
        integrity_msg,
    )


def drop_outliers(values: list[float]) -> list[float]:
    if len(values) < 3:
        return values
    med = statistics.median(values)
    cap = max(med * 2.0, med + 75.0)
    return [v for v in values if v <= cap]


def robust_median(values: list[float]) -> Optional[float]:
    values = drop_outliers(values)
    if not values:
        return None
    if len(values) < 4:
        return statistics.median(values)
    s = sorted(values)
    lo = len(s) // 4
    hi = max(lo + 1, (3 * len(s)) // 4)
    return statistics.median(s[lo:hi])


@lru_cache(maxsize=8)
def supports_inprocess_bench(astral: str) -> bool:
    try:
        proc = subprocess.run(
            [astral, "--help"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False
    blob = proc.stdout + proc.stderr
    return "--time-sql-runs" in blob


@lru_cache(maxsize=8)
def supports_time_sql_setup(astral: str) -> bool:
    try:
        proc = subprocess.run(
            [astral, "--help"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False
    blob = proc.stdout + proc.stderr
    return "--time-sql-setup" in blob


def _run_cmd(
    cmd: list[str],
    timeout_s: float,
    cwd: Optional[Path],
    time_file: Optional[Path] = None,
    env: Optional[dict[str, str]] = None,
) -> tuple[int, str, float]:
    run_env = dict(env) if env is not None else os.environ.copy()
    if time_file is not None:
        if time_file.exists():
            time_file.unlink()
        run_env["ASTRALDB_TIME_SQL_OUT"] = str(time_file)
    t0 = time.perf_counter()
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout_s,
        check=False,
        cwd=str(cwd) if cwd else None,
        env=run_env,
    )
    wall_ms = (time.perf_counter() - t0) * 1000.0
    blob = proc.stdout + "\n" + proc.stderr
    if time_file is not None and time_file.is_file():
        blob = blob + "\n" + time_file.read_text(encoding="utf-8", errors="replace")
    log_path = Path.cwd() / "astraldb.log"
    if cwd is not None:
        log_path = cwd / "astraldb.log"
    if log_path.is_file():
        try:
            tail = log_path.read_text(encoding="utf-8", errors="replace")[-8192:]
            for line in tail.splitlines():
                if "parse+compile_ms=" in line and "execute_ms=" in line:
                    blob = blob + "\n" + line
        except OSError:
            pass
    return proc.returncode, blob, wall_ms


def run_time_sql(
    astral: Path,
    sql_path: Path,
    opt: str,
    timeout_s: float,
    cwd: Optional[Path] = None,
    memory: bool = False,
    warmup: int = 0,
    runs: int = 1,
    setup_path: Optional[Path] = None,
    durable: bool = False,
    env: Optional[dict[str, str]] = None,
    database: Optional[Path] = None,
) -> SqlTiming:
    base: list[str] = [str(astral)]
    if memory:
        base.append("-m")
    base.append(opt)
    if database is not None:
        base += ["--database", str(database)]

    use_bench = (warmup > 0 or runs > 1) and supports_inprocess_bench(str(astral))
    split_setup = setup_path is not None and setup_path.is_file()
    inprocess_setup = split_setup and supports_time_sql_setup(str(astral))

    def seed_db_once() -> bool:
        if not split_setup or inprocess_setup or setup_path is None:
            return True
        try:
            proc = subprocess.run(
                base + [str(setup_path)],
                capture_output=True,
                text=True,
                timeout=timeout_s,
                check=False,
                cwd=str(cwd) if cwd else None,
            )
        except subprocess.TimeoutExpired:
            return False
        return proc.returncode == 0

    with tempfile.NamedTemporaryFile(
        mode="w+", suffix=".time", delete=False, encoding="utf-8"
    ) as tf:
        time_path = Path(tf.name)
    try:
        if use_bench:
            cmd = base + [
                "--time-sql-warmup",
                str(warmup),
                "--time-sql-runs",
                str(runs),
            ]
            if inprocess_setup:
                cmd += ["--time-sql-setup", str(setup_path)]
            if durable:
                cmd.append("--time-sql-durable")
            cmd += ["--time-sql", str(sql_path)]
            try:
                rc, blob, _wall_ms = _run_cmd(cmd, timeout_s, cwd, time_path, env)
            except subprocess.TimeoutExpired:
                return SqlTiming(None, None, None, None, None, None, None, None)
            timing = parse_time_sql_output(blob)
            if timing.stable_ms() is not None and rc == 0:
                return timing

        vals: list[float] = []
        compiles: list[float] = []

        def simple_once() -> SqlTiming:
            if time_path.exists():
                time_path.unlink()
            cmd = base + ["--time-sql", str(sql_path)]
            if durable:
                cmd.append("--time-sql-durable")
            try:
                rc, blob, wall_ms = _run_cmd(cmd, timeout_s, cwd, time_path, env)
            except subprocess.TimeoutExpired:
                return SqlTiming(None, None, None, None, None, None, None, None)
            timing = parse_time_sql_output(blob)
            if timing.stable_ms() is not None and rc == 0:
                return timing
            if rc == 0:
                return SqlTiming(None, wall_ms, None, None, wall_ms, None, None, None)
            return SqlTiming(None, None, None, None, None, None, None, None)

        if not seed_db_once():
            return SqlTiming(None, None, None, None, None, None, None, None)

        for _ in range(max(0, warmup)):
            simple_once()

        for _ in range(max(1, runs)):
            t = simple_once()
            if t.stable_ms() is not None:
                vals.append(t.stable_ms())
            if t.compile_ms is not None:
                compiles.append(t.compile_ms)

        trimmed = drop_outliers(vals)
        med = robust_median(trimmed)
        if med is None:
            return SqlTiming(None, None, None, None, None, None, None, None)
        compile_med = statistics.median(compiles) if compiles else None
        return SqlTiming(compile_med, med, min(trimmed) if trimmed else None, max(trimmed) if trimmed else None, med)
    finally:
        time_path.unlink(missing_ok=True)
