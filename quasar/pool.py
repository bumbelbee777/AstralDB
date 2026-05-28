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
from quasar.paths import path_key
from quasar.errors import QuasarOverloadError
from quasar.mvcc import MvccConfig, is_write_sql, prepare_read_sql
from quasar.recovery import RetryPolicy, RetryStats, execute_with_retry, rollback_database
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
    rollback_on_batch_failure: bool = True
    read_lane_immediate: bool = True
    per_db_max_inflight: int = 2
    keepalive_interval_sec: float = 0.0
    overload_soft_limit_ratio: float = 0.75
    overload_hard_limit_ratio: float = 0.95
    overload_mode: str = "fail_fast"

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
    is_write: bool


class PooledAstralDBClient(AstralDBClient):
    """Batched subprocess pool — see docs/Quasar.md."""

    def __init__(
        self,
        executable: Optional[PathLike] = None,
        *,
        pool: Optional[PoolConfig] = None,
        security_policy: Optional[SecurityPolicy] = None,
        mvcc: Optional[MvccConfig] = None,
        retry_policy: Optional[RetryPolicy] = None,
        **kwargs,
    ) -> None:
        super().__init__(executable, security_policy=security_policy, **kwargs)
        self.pool_config = pool or PoolConfig()
        self.mvcc = mvcc or MvccConfig()
        self.retry_policy = retry_policy or RetryPolicy(max_retries=0)
        self.retry_stats = RetryStats()
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
        self._db_slots: Dict[str, threading.Semaphore] = {}
        self._db_slot_lock = threading.Lock()
        self._keepalive_stop = threading.Event()
        self._keepalive_thread: Optional[threading.Thread] = None
        if self.pool_config.keepalive_interval_sec > 0:
            self._start_keepalive()
        self.stats: Dict[str, int] = {
            "queries_submitted": 0,
            "batches_executed": 0,
            "statements_batched": 0,
            "immediate_queries": 0,
            "read_lane_queries": 0,
            "rejected_queue_full": 0,
            "batch_rollbacks": 0,
            "keepalive_pings": 0,
            "shed_soft_limit": 0,
            "shed_hard_limit": 0,
        }
        self._timings: Dict[str, float] = {
            "queue_wait_ms_total": 0.0,
            "queue_wait_ms_max": 0.0,
            "queue_wait_samples": 0.0,
            "batch_elapsed_ms_total": 0.0,
            "batch_elapsed_ms_max": 0.0,
        }

    def _db_semaphore(self, database: Path) -> threading.Semaphore:
        key = path_key(database)
        with self._db_slot_lock:
            if key not in self._db_slots:
                n = max(1, int(self.pool_config.per_db_max_inflight))
                self._db_slots[key] = threading.Semaphore(n)
            return self._db_slots[key]

    def _with_db_slot(self, database: Path):
        from contextlib import contextmanager

        sem = self._db_semaphore(database)

        @contextmanager
        def _guard():
            sem.acquire()
            try:
                yield
            finally:
                sem.release()

        return _guard()

    def _start_keepalive(self) -> None:
        interval = self.pool_config.keepalive_interval_sec

        def _loop() -> None:
            while not self._keepalive_stop.is_set():
                for key in list(self._warmed):
                    try:
                        super(PooledAstralDBClient, self).query(
                            "SELECT 1;", database=Path(key), immediate=True
                        )
                        self.stats["keepalive_pings"] += 1
                    except RuntimeError:
                        pass
                self._keepalive_stop.wait(interval)

        self._keepalive_thread = threading.Thread(target=_loop, name="quasar-keepalive", daemon=True)
        self._keepalive_thread.start()

    def heal(self) -> None:
        """Post-crash: drain pending queue and ping warmed databases."""
        self._drain_once()
        for key in list(self._warmed):
            try:
                rollback_database(self, Path(key))
            except Exception:
                pass
            try:
                super(PooledAstralDBClient, self).query("SELECT 1;", database=Path(key), immediate=True)
            except RuntimeError:
                pass

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
            key = path_key(item.database)
            bucket.setdefault(key, []).append(item)
            if sum(len(v) for v in bucket.values()) >= self.pool_config.batch_max_statements:
                break

        for items in bucket.values():
            self._executor.submit(self._execute_batch, items)

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

    def _execute_batch(self, items: List[_PendingQuery]) -> None:
        if not items:
            return
        self._inflight_sem.acquire()
        try:
            database = items[0].database
            statements = [it.sql for it in items]
            with self._with_db_slot(database):
                batch_start = time.perf_counter()

                def _run_once() -> QueryResult:
                    if self.pool_config.combine_transactions and len(statements) > 1:
                        body = "BEGIN;\n" + "\n".join(statements) + "\nCOMMIT;"
                    else:
                        body = "\n".join(statements)
                    start = time.perf_counter()
                    cmd = self._base_cmd(database) + ["-q", body]
                    proc = self._run_subprocess(cmd)
                    elapsed_ms = (time.perf_counter() - start) * 1000.0
                    return QueryResult(
                        stdout=proc.stdout or "",
                        stderr=proc.stderr or "",
                        returncode=proc.returncode,
                        elapsed_ms=elapsed_ms,
                        command=cmd,
                    )

                if self.retry_policy.max_retries > 0:
                    result = execute_with_retry(_run_once, self.retry_policy, stats=self.retry_stats)
                else:
                    result = _run_once()
                batch_elapsed_ms = (time.perf_counter() - batch_start) * 1000.0
                self._timings["batch_elapsed_ms_total"] += batch_elapsed_ms
                self._timings["batch_elapsed_ms_max"] = max(
                    self._timings["batch_elapsed_ms_max"], batch_elapsed_ms
                )

            self.stats["batches_executed"] += 1
            self.stats["statements_batched"] += len(items)
            per_stmt_ms = result.elapsed_ms / max(len(items), 1)

            if not result.ok and self.pool_config.rollback_on_batch_failure:
                rollback_database(self, database)
                self.stats["batch_rollbacks"] += 1

            for it in items:
                queue_wait_ms = max(0.0, (time.perf_counter() - it.submitted_at) * 1000.0)
                self._timings["queue_wait_ms_total"] += queue_wait_ms
                self._timings["queue_wait_ms_max"] = max(self._timings["queue_wait_ms_max"], queue_wait_ms)
                self._timings["queue_wait_samples"] += 1.0
                individual = QueryResult(
                    stdout=result.stdout,
                    stderr=result.stderr,
                    returncode=result.returncode,
                    elapsed_ms=per_stmt_ms,
                    command=result.command,
                )
                if not individual.ok:
                    it.future.set_exception(
                        RuntimeError(individual.stderr or individual.stdout or "batch failed")
                    )
                else:
                    it.future.set_result(individual)
        except Exception as exc:
            if items and self.pool_config.rollback_on_batch_failure:
                rollback_database(self, items[0].database)
                self.stats["batch_rollbacks"] += 1
            for it in items:
                if not it.future.done():
                    it.future.set_exception(exc)
        finally:
            self._inflight_sem.release()

    def warm(self, databases: Sequence[PathLike]) -> None:
        for db in databases:
            key = path_key(db)
            with self._warm_lock:
                if key in self._warmed:
                    continue
                self._warmed.add(key)
                self._db_semaphore(Path(db))
            try:
                self.query("SELECT 1;", database=db, immediate=True)
            except RuntimeError:
                pass

    def _read_lane(self, sql: str, database: PathLike) -> QueryResult:
        """Immediate subprocess with optional AstralDB snapshot transaction."""
        self.stats["read_lane_queries"] += 1
        body = prepare_read_sql(sql, self.mvcc) if self.mvcc.snapshot_reads else sql

        def _run() -> QueryResult:
            with self._with_db_slot(Path(database)):
                return super(PooledAstralDBClient, self).query(body, database=database, immediate=True)

        if self.retry_policy.max_retries > 0:
            return execute_with_retry(_run, self.retry_policy, stats=self.retry_stats)
        return _run()

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
        if memory or database is None:
            self.stats["immediate_queries"] += 1
            return super().query(sql, database=database, memory=memory, immediate=True)

        read_lane = (
            self.pool_config.read_lane_immediate
            and self.mvcc.read_immediate
            and not is_write_sql(sql)
        )
        if immediate or read_lane:
            self.stats["immediate_queries"] += 1
            if read_lane and not immediate:
                return self._read_lane(sql, database)
            if self.retry_policy.max_retries > 0:

                def _run() -> QueryResult:
                    with self._with_db_slot(Path(database)):
                        return super(PooledAstralDBClient, self).query(
                            sql, database=database, immediate=True
                        )

                return execute_with_retry(_run, self.retry_policy, stats=self.retry_stats)
            with self._with_db_slot(Path(database)):
                return super().query(sql, database=database, immediate=True)

        fut: Future = Future()
        pending = _PendingQuery(
            sql=sql,
            database=Path(database).resolve(),
            future=fut,
            submitted_at=time.perf_counter(),
            is_write=True,
        )
        queue_depth = self._pending.qsize()
        hard_limit = int(self.pool_config.max_queue * self.pool_config.overload_hard_limit_ratio)
        soft_limit = int(self.pool_config.max_queue * self.pool_config.overload_soft_limit_ratio)
        if queue_depth >= hard_limit:
            self.stats["shed_hard_limit"] += 1
            raise QuasarOverloadError("hard overload limit reached; shedding requests")
        if (
            queue_depth >= soft_limit
            and self.pool_config.overload_mode == "fail_fast"
            and not pending.is_write
        ):
            self.stats["shed_soft_limit"] += 1
            raise QuasarOverloadError("soft overload limit reached; read request shed")

        def _enqueue() -> None:
            self._pending.put_nowait(pending)

        try:
            if self.retry_policy.max_retries > 0:
                execute_with_retry(_enqueue, self.retry_policy, stats=self.retry_stats)
            else:
                _enqueue()
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
        out = {k: float(v) for k, v in self.stats.items()}
        samples = max(1.0, self._timings["queue_wait_samples"])
        out["queue_wait_ms_mean"] = self._timings["queue_wait_ms_total"] / samples
        out["queue_wait_ms_max"] = self._timings["queue_wait_ms_max"]
        out["batch_elapsed_ms_mean"] = self._timings["batch_elapsed_ms_total"] / max(
            1.0, out.get("batches_executed", 0.0)
        )
        out["batch_elapsed_ms_max"] = self._timings["batch_elapsed_ms_max"]
        out["queue_depth"] = float(self._pending.qsize())
        out.update({f"retry_{k}": v for k, v in self.retry_stats.to_dict().items()})
        return out

    def close(self, *, drain_timeout_sec: float = 5.0) -> None:
        self._keepalive_stop.set()
        if self._keepalive_thread:
            self._keepalive_thread.join(timeout=2.0)
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
    recovery_config: Optional[Dict] = None,
    mvcc_config: Optional[Dict] = None,
    **kwargs,
) -> AstralDBClient:
    policy = security_policy or SecurityPolicy()
    retry = RetryPolicy.from_config(recovery_config)
    mvcc = MvccConfig.from_config(mvcc_config)
    if not use_pool:
        client = AstralDBClient(executable, security_policy=policy, **kwargs)
        client._quasar_retry_policy = retry  # type: ignore[attr-defined]
        client._quasar_mvcc = mvcc  # type: ignore[attr-defined]
        return client
    cfg = PoolConfig.from_dict(pool_config or {})
    return PooledAstralDBClient(
        executable,
        pool=cfg,
        security_policy=policy,
        mvcc=mvcc,
        retry_policy=retry,
        **kwargs,
    )
