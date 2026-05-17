"""High-throughput AstralDB access: batching, pooling, and per-database session lanes."""

from __future__ import annotations

import queue
import threading
import time
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Union

from quasar.client import AstralDBClient, QueryResult
from quasar.errors import QuasarOverloadError
from quasar.security import SecurityPolicy, clamp_pool_config, validate_sql

PathLike = Union[str, Path]


@dataclass
class PoolConfig:
    max_workers: int = 32
    batch_max_statements: int = 48
    batch_window_ms: float = 3.0
    max_inflight_batches: int = 24
    max_queue: int = 10_000
    warm_on_start: bool = True
    combine_transactions: bool = True

    @classmethod
    def from_dict(cls, raw: Dict) -> "PoolConfig":
        clamped = clamp_pool_config(raw)
        fields = {f for f in cls.__dataclass_fields__}  # type: ignore[attr-defined]
        return cls(**{k: clamped[k] for k in clamped if k in fields})


@dataclass
class _PendingQuery:
    sql: str
    database: Path
    future: Future
    submitted_at: float


class PooledAstralDBClient(AstralDBClient):
    """Batched subprocess pool — see docs/Quasar.md § Production."""

    def __init__(
        self,
        executable: Optional[PathLike] = None,
        *,
        pool: Optional[PoolConfig] = None,
        security_policy: Optional[SecurityPolicy] = None,
        **kwargs,
    ) -> None:
        super().__init__(executable, security_policy=security_policy, **kwargs)
        self.pool_config = pool or PoolConfig()
        self._pending: queue.Queue[_PendingQuery] = queue.Queue(maxsize=self.pool_config.max_queue)
        self._inflight_sem = threading.Semaphore(self.pool_config.max_inflight_batches)
        self._stop = threading.Event()
        self._executor = ThreadPoolExecutor(
            max_workers=self.pool_config.max_workers,
            thread_name_prefix="quasar-pool",
        )
        self._flusher = threading.Thread(target=self._flush_loop, name="quasar-flusher", daemon=True)
        self._flusher.start()
        self._warmed: set[str] = set()
        self._warm_lock = threading.Lock()
        self.stats: Dict[str, int] = {
            "queries_submitted": 0,
            "batches_executed": 0,
            "statements_batched": 0,
            "immediate_queries": 0,
            "rejected_queue_full": 0,
        }

    def _flush_loop(self) -> None:
        while not self._stop.is_set():
            self._drain_once()
            time.sleep(self.pool_config.batch_window_ms / 1000.0)

    def _drain_once(self) -> None:
        deadline = time.perf_counter() + (self.pool_config.batch_window_ms / 1000.0)
        bucket: Dict[str, List[_PendingQuery]] = {}
        while (
            time.perf_counter() < deadline
            and sum(len(v) for v in bucket.values()) < self.pool_config.batch_max_statements
        ):
            try:
                item = self._pending.get_nowait()
            except queue.Empty:
                if bucket:
                    break
                return
            key = str(item.database.resolve())
            bucket.setdefault(key, []).append(item)
            if sum(len(v) for v in bucket.values()) >= self.pool_config.batch_max_statements:
                break

        for items in bucket.values():
            self._executor.submit(self._execute_batch, items)

    def _execute_batch(self, items: List[_PendingQuery]) -> None:
        if not items:
            return
        self._inflight_sem.acquire()
        try:
            database = items[0].database
            statements = [it.sql for it in items]
            if self.pool_config.combine_transactions and len(statements) > 1:
                body = "BEGIN;\n" + "\n".join(statements) + "\nCOMMIT;"
            else:
                body = "\n".join(statements)
            start = time.perf_counter()
            cmd = self._base_cmd(database) + ["-q", body]
            proc = self._run_subprocess(cmd)
            elapsed_ms = (time.perf_counter() - start) * 1000.0
            result = QueryResult(
                stdout=proc.stdout or "",
                stderr=proc.stderr or "",
                returncode=proc.returncode,
                elapsed_ms=elapsed_ms,
                command=cmd,
            )
            self.stats["batches_executed"] += 1
            self.stats["statements_batched"] += len(items)
            per_stmt_ms = elapsed_ms / max(len(items), 1)
            for it in items:
                individual = QueryResult(
                    stdout=result.stdout,
                    stderr=result.stderr,
                    returncode=result.returncode,
                    elapsed_ms=per_stmt_ms,
                    command=cmd,
                )
                if not individual.ok:
                    it.future.set_exception(
                        RuntimeError(individual.stderr or individual.stdout or "batch failed")
                    )
                else:
                    it.future.set_result(individual)
        except Exception as exc:
            for it in items:
                if not it.future.done():
                    it.future.set_exception(exc)
        finally:
            self._inflight_sem.release()

    def _run_subprocess(self, cmd: List[str]):
        import subprocess

        return subprocess.run(
            cmd,
            cwd=str(self.cwd) if self.cwd else None,
            capture_output=True,
            text=True,
            timeout=self.timeout_sec,
            shell=False,
        )

    def warm(self, databases: Sequence[PathLike]) -> None:
        for db in databases:
            key = str(Path(db).resolve())
            with self._warm_lock:
                if key in self._warmed:
                    continue
                self._warmed.add(key)
            try:
                self.query("SELECT 1;", database=db, immediate=True)
            except RuntimeError:
                pass

    def query(
        self,
        sql: str,
        *,
        database: Optional[PathLike] = None,
        memory: bool = False,
        immediate: bool = False,
    ) -> QueryResult:
        validate_sql(sql, self.security)
        self.stats["queries_submitted"] += 1
        if memory or database is None or immediate:
            self.stats["immediate_queries"] += 1
            return super().query(sql, database=database, memory=memory, immediate=True)

        fut: Future = Future()
        pending = _PendingQuery(sql=sql, database=Path(database).resolve(), future=fut, submitted_at=time.perf_counter())
        try:
            self._pending.put_nowait(pending)
        except queue.Full:
            self.stats["rejected_queue_full"] += 1
            raise QuasarOverloadError(
                f"pool queue full ({self.pool_config.max_queue}); retry or increase pool.max_queue"
            )
        result = fut.result(timeout=self.timeout_sec)
        if not result.ok:
            result.raise_on_error(redact_secrets=self.security.redact_secrets_in_errors)
        return result

    def run(
        self,
        args,
        *,
        database=None,
        memory=False,
        input_text=None,
        timeout_sec=None,
        immediate: bool = True,
    ) -> QueryResult:
        if not immediate:
            raise NotImplementedError("pooled run() only supports immediate=True")
        return super().run(
            args,
            database=database,
            memory=memory,
            input_text=input_text,
            timeout_sec=timeout_sec,
        )

    def pool_stats(self) -> Dict[str, float]:
        return {k: float(v) for k, v in self.stats.items()}

    def close(self, *, drain_timeout_sec: float = 5.0) -> None:
        self._stop.set()
        deadline = time.perf_counter() + drain_timeout_sec
        while time.perf_counter() < deadline:
            if self._pending.empty():
                break
            self._drain_once()
            time.sleep(0.01)
        remaining: List[_PendingQuery] = []
        while True:
            try:
                remaining.append(self._pending.get_nowait())
            except queue.Empty:
                break
        if remaining:
            self._execute_batch(remaining)
        self._flusher.join(timeout=2.0)
        self._executor.shutdown(wait=True, cancel_futures=False)


def create_client(
    executable: Optional[PathLike] = None,
    *,
    pool_config: Optional[Dict] = None,
    use_pool: bool = True,
    security_policy: Optional[SecurityPolicy] = None,
    **kwargs,
) -> AstralDBClient:
    policy = security_policy or SecurityPolicy()
    if not use_pool:
        return AstralDBClient(executable, security_policy=policy, **kwargs)
    cfg = PoolConfig.from_dict(pool_config or {})
    return PooledAstralDBClient(executable, pool=cfg, security_policy=policy, **kwargs)
