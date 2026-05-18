"""Recovery retries, MVCC snapshot reads, and workload half-open circuits."""

from __future__ import annotations

import time
from pathlib import Path

import pytest

from quasar.errors import QuasarCircuitOpenError, QuasarOverloadError
from quasar.mvcc import MvccConfig, is_write_sql, prepare_read_sql, wrap_snapshot_transaction
from quasar.pool import PooledAstralDBClient, PoolConfig
from quasar.recovery import RetryPolicy, RetryStats, execute_with_retry, is_transient_error
from quasar.workload import CircuitBreakerConfig, ShardCircuitBreaker, WorkloadGuard


def test_is_transient_error():
    assert is_transient_error(QuasarOverloadError("pool queue full"))
    assert is_transient_error(RuntimeError("AstralDB timed out after 5s"))
    assert not is_transient_error(ValueError("syntax error"))


def test_execute_with_retry_succeeds_after_failure():
    stats = RetryStats()
    policy = RetryPolicy(max_retries=3, base_delay_ms=1, max_delay_ms=5, jitter=False)
    calls = {"n": 0}

    def flaky():
        calls["n"] += 1
        if calls["n"] < 2:
            raise RuntimeError("timeout")
        return "ok"

    assert execute_with_retry(flaky, policy, stats=stats) == "ok"
    assert stats.retries == 1
    assert stats.successes_after_retry == 1


def test_wrap_snapshot_transaction():
    wrapped = wrap_snapshot_transaction("SELECT 1")
    assert wrapped.startswith("BEGIN;")
    assert "SELECT 1" in wrapped
    assert wrapped.endswith("COMMIT;")


def test_prepare_read_sql_temporal():
    cfg = MvccConfig(snapshot_reads=False, temporal_as_of="2020-01-01")
    sql = prepare_read_sql("SELECT * FROM t", cfg)
    assert "FOR SYSTEM TIME AS OF" in sql


def test_is_write_sql():
    assert not is_write_sql("SELECT 1;")
    assert is_write_sql("INSERT INTO t VALUES (1);")


def test_pooled_read_lane_uses_snapshot(mock_astraldb, tmp_path: Path):
    db = tmp_path / "read.db"
    client = PooledAstralDBClient(
        mock_astraldb,
        pool=PoolConfig(batch_window_ms=50, read_lane_immediate=True),
        mvcc=MvccConfig(snapshot_reads=True),
        timeout_sec=5.0,
    )
    result = client.query("SELECT 1;", database=db, immediate=False)
    assert result.ok
    stats = client.pool_stats()
    assert stats.get("read_lane_queries", 0) >= 1
    client.close()


def test_circuit_half_open_allows_probe():
    breaker = ShardCircuitBreaker(
        CircuitBreakerConfig(failure_threshold=1, cooldown_sec=0.05, half_open_max_probes=1)
    )
    breaker.record_failure("s0")
    assert not breaker.allow("s0")
    time.sleep(0.06)
    assert breaker.allow("s0")
    breaker.record_success("s0")
    assert breaker.allow("s0")


def test_workload_guard_run_retries(mock_client, shard_config: dict):
    from quasar.quasar import QuasarShard

    shard = QuasarShard.from_config(shard_config, client=mock_client)
    policy = RetryPolicy(max_retries=2, base_delay_ms=1, max_delay_ms=5, jitter=False)
    guard = WorkloadGuard(retry_policy=policy)
    node = shard.nodes[0]
    node.database.parent.mkdir(parents=True, exist_ok=True)

    calls = {"n": 0}

    def flaky():
        calls["n"] += 1
        if calls["n"] < 2:
            raise RuntimeError("temporarily unavailable")
        return shard.execute("SELECT 1;", shard_key="user:1")

    guard.run(node.name, flaky)
    assert calls["n"] == 2


def test_workload_circuit_open_raises():
    breaker = ShardCircuitBreaker(CircuitBreakerConfig(failure_threshold=1, cooldown_sec=60))
    guard = WorkloadGuard(breaker)
    breaker.record_failure("x")
    with pytest.raises(QuasarCircuitOpenError):
        guard.run("x", lambda: 1)
