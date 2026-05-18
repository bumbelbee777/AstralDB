"""Subprocess wrapper around the AstralDB CLI executable."""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence, Union

from quasar.paths import path_for_cli
from quasar.security import (
    SecurityPolicy,
    redact_command,
    validate_executable,
    validate_sql,
)

PathLike = Union[str, Path]


@dataclass
class QueryResult:
    """Outcome of a single AstralDB CLI invocation."""

    stdout: str
    stderr: str
    returncode: int
    elapsed_ms: float
    command: List[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.returncode == 0

    def raise_on_error(self, *, redact_secrets: bool = True) -> "QueryResult":
        if not self.ok:
            detail = self.stderr.strip() or self.stdout.strip() or "unknown error"
            cmd_display = redact_command(self.command) if redact_secrets else self.command
            raise RuntimeError(
                f"AstralDB exited {self.returncode}: {detail}\n  cmd: {' '.join(cmd_display)}"
            )
        return self


def find_astraldb(explicit: Optional[PathLike] = None) -> Path:
    """Resolve the AstralDB executable (env, explicit path, repo build dirs, PATH)."""
    if explicit is not None:
        return validate_executable(Path(explicit))

    for env_key in ("QUASAR_ASTRALDB", "ASTRALDB_BIN"):
        env_val = os.environ.get(env_key)
        if env_val:
            return validate_executable(Path(env_val))

    repo_root = Path(__file__).resolve().parents[1]
    candidates: List[Path] = [
        repo_root / "bin" / "astraldb.exe",
        repo_root / "bin" / "astraldb",
        repo_root / "build-cmake" / "Release" / "astraldb.exe",
        repo_root / "build-cmake" / "Release" / "astraldb",
        repo_root / "build-ci" / "astraldb.exe",
        repo_root / "build-ci" / "astraldb",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return validate_executable(candidate)

    which = shutil.which("astraldb")
    if which:
        return validate_executable(Path(which))

    raise FileNotFoundError(
        "Could not locate AstralDB. Set QUASAR_ASTRALDB, pass --astraldb, or build the project."
    )


class AstralDBClient:
    """Run AstralDB through its CLI (`-q`, `-s`, export/import bundle, etc.)."""

    def __init__(
        self,
        executable: Optional[PathLike] = None,
        *,
        cwd: Optional[PathLike] = None,
        user: Optional[str] = None,
        password: Optional[str] = None,
        optimization: str = "O2",
        timeout_sec: Optional[float] = 300.0,
        extra_args: Optional[Sequence[str]] = None,
        security_policy: Optional[SecurityPolicy] = None,
        audit_file: Optional[PathLike] = None,
    ) -> None:
        self.executable = find_astraldb(executable)
        self.cwd = Path(cwd).resolve() if cwd else None
        self.user = user or os.environ.get("ASTRALDB_USER")
        self.password = password or os.environ.get("ASTRALDB_PASSWORD")
        self.audit_file = Path(audit_file).resolve() if audit_file else None
        if optimization not in ("O0", "O1", "O2", "O3"):
            raise ValueError("optimization must be O0, O1, O2, or O3")
        self.optimization = optimization
        self.timeout_sec = timeout_sec
        self.extra_args = list(extra_args or [])
        self.security = security_policy or SecurityPolicy()

    def _base_cmd(self, database: Optional[PathLike] = None, *, memory: bool = False) -> List[str]:
        cmd = [str(self.executable), f"-{self.optimization}"]
        cmd.extend(self.extra_args)
        if database is not None:
            db_path = Path(database).resolve()
            cmd.extend(["--database", path_for_cli(db_path)])
        elif memory:
            cmd.append("-m")
        if self.user:
            cmd.extend(["-U", self.user])
        if self.password:
            cmd.extend(["-P", self.password])
        if self.audit_file is not None:
            cmd.extend(["--audit-file", str(self.audit_file)])
        return cmd

    def run(
        self,
        args: Sequence[str],
        *,
        database: Optional[PathLike] = None,
        memory: bool = False,
        input_text: Optional[str] = None,
        timeout_sec: Optional[float] = None,
    ) -> QueryResult:
        if any("\x00" in str(a) for a in args):
            raise ValueError("null byte in command arguments")
        cmd = self._base_cmd(database, memory=memory) + list(args)
        start = time.perf_counter()
        try:
            proc = subprocess.run(
                cmd,
                cwd=str(self.cwd) if self.cwd else None,
                input=input_text,
                capture_output=True,
                text=True,
                timeout=timeout_sec if timeout_sec is not None else self.timeout_sec,
                shell=False,
            )
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"AstralDB timed out after {self.timeout_sec}s") from exc
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        return QueryResult(
            stdout=proc.stdout or "",
            stderr=proc.stderr or "",
            returncode=proc.returncode,
            elapsed_ms=elapsed_ms,
            command=cmd,
        )

    def query(
        self,
        sql: str,
        *,
        database: Optional[PathLike] = None,
        memory: bool = False,
        immediate: bool = False,
    ) -> QueryResult:
        del immediate
        validate_sql(sql, self.security)
        return self.run(["-q", sql], database=database, memory=memory).raise_on_error(
            redact_secrets=self.security.redact_secrets_in_errors
        )

    def script(
        self,
        path: PathLike,
        *,
        database: Optional[PathLike] = None,
        memory: bool = False,
    ) -> QueryResult:
        script_path = Path(path).resolve()
        if not script_path.is_file():
            raise FileNotFoundError(f"script not found: {script_path}")
        return self.run(["-s", path_for_cli(script_path)], database=database, memory=memory).raise_on_error(
            redact_secrets=self.security.redact_secrets_in_errors
        )

    def checkpoint_sql(self, *, database: PathLike) -> QueryResult:
        return self.query("BEGIN; COMMIT;", database=database, immediate=True)

    def query_snapshot(
        self,
        sql: str,
        *,
        database: Optional[PathLike] = None,
        memory: bool = False,
    ) -> QueryResult:
        """Run a read inside AstralDB's BEGIN snapshot (one consistent view per call)."""
        from quasar.mvcc import prepare_read_sql, MvccConfig

        mvcc = getattr(self, "_quasar_mvcc", None) or MvccConfig()
        body = prepare_read_sql(sql, mvcc)
        return self.query(body, database=database, memory=memory, immediate=True)

    def query_with_retry(
        self,
        sql: str,
        *,
        database: Optional[PathLike] = None,
        memory: bool = False,
        immediate: bool = False,
    ) -> QueryResult:
        from quasar.recovery import RetryPolicy, execute_with_retry

        policy = getattr(self, "_quasar_retry_policy", None) or RetryPolicy()
        if policy.max_retries <= 0:
            return self.query(sql, database=database, memory=memory, immediate=immediate)

        return execute_with_retry(
            lambda: self.query(sql, database=database, memory=memory, immediate=immediate),
            policy,
        )

    def export_bundle(
        self,
        bundle_path: PathLike,
        *,
        database: PathLike,
        fmt: str = "json",
    ) -> QueryResult:
        fmt = fmt.lower()
        if fmt not in ("json", "csv", "tsv"):
            raise ValueError("export format must be json, csv, or tsv")
        return self.run(
            ["--export-bundle", str(Path(bundle_path).resolve()), "--export-format", fmt],
            database=database,
        ).raise_on_error(redact_secrets=self.security.redact_secrets_in_errors)

    def import_bundle(
        self,
        bundle_path: PathLike,
        *,
        database: PathLike,
        fmt: str = "json",
    ) -> QueryResult:
        fmt = fmt.lower()
        if fmt not in ("json", "csv", "tsv"):
            raise ValueError("import format must be json, csv, or tsv")
        return self.run(
            ["--import-bundle", str(Path(bundle_path).resolve()), "--import-format", fmt],
            database=database,
        ).raise_on_error(redact_secrets=self.security.redact_secrets_in_errors)

    def version(self) -> str:
        result = self.run(["-v"])
        result.raise_on_error(redact_secrets=self.security.redact_secrets_in_errors)
        return result.stdout.strip()

    def health(self, *, database: Optional[PathLike] = None, memory: bool = False) -> bool:
        try:
            self.query("SELECT 1;", database=database, memory=memory, immediate=True)
            return True
        except RuntimeError:
            return False

    def compile_sql(
        self,
        sql_path: PathLike,
        output: PathLike,
        *,
        compile_pool: bool = False,
    ) -> QueryResult:
        args = ["-cc", str(Path(sql_path).resolve()), "-o", str(Path(output).resolve())]
        if compile_pool:
            args.append("-cp")
        return self.run(args).raise_on_error(redact_secrets=self.security.redact_secrets_in_errors)

    def time_sql(self, sql_path: PathLike, *, database: Optional[PathLike] = None) -> QueryResult:
        return self.run(
            ["--time-sql", str(Path(sql_path).resolve())], database=database
        ).raise_on_error(redact_secrets=self.security.redact_secrets_in_errors)

    def convert_file(
        self,
        src: PathLike,
        dst: PathLike,
        *,
        from_fmt: str = "csv",
        to_fmt: str = "json",
    ) -> QueryResult:
        from_fmt = from_fmt.lower()
        to_fmt = to_fmt.lower()
        for fmt in (from_fmt, to_fmt):
            if fmt not in ("csv", "json", "tsv"):
                raise ValueError("convert format must be csv, json, or tsv")
        return self.run(
            [
                "--convert",
                str(Path(src).resolve()),
                str(Path(dst).resolve()),
                "--from",
                from_fmt,
                "--to",
                to_fmt,
            ],
            memory=True,
        ).raise_on_error(redact_secrets=self.security.redact_secrets_in_errors)
