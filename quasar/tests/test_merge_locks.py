"""Tests for merge, locks, restore guard, and replica drift."""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from quasar.client import AstralDBClient, QueryResult
from quasar.drift import QuasarDrift
from quasar.errors import QuasarRestoreError
from quasar.locks import DatabaseLock, lock_databases
from quasar.merge import looks_like_read_query, merge_routed_results
from quasar.quasar import QuasarBackup, QuasarCluster, QuasarReplica, QuasarShard, ReplicaSet, RoutedResult


def test_looks_like_read_query():
    assert looks_like_read_query("SELECT 1;")
    assert looks_like_read_query("  WITH cte AS (SELECT 1) SELECT * FROM cte")
    assert not looks_like_read_query("INSERT INTO t VALUES (1);")
    assert not looks_like_read_query("UPDATE t SET x=1;")


def test_merge_routed_results():
    results = [
        RoutedResult(
            node="a",
            database=Path("a.db"),
            result=QueryResult("row1\n", "", 0, 1.0),
        ),
        RoutedResult(
            node="b",
            database=Path("b.db"),
            result=QueryResult("row2\n", "", 0, 1.0),
        ),
    ]
    merged = merge_routed_results(results, sql="SELECT 1;")
    assert merged.shard_count == 2
    assert "row1" in merged.merged_stdout and "row2" in merged.merged_stdout
    assert merged.all_ok

    with pytest.raises(ValueError, match="cannot merge"):
        merge_routed_results(results, sql="INSERT INTO t VALUES (1);")


def test_restore_quiesce_blocks_recent_wal(tmp_path: Path):
    db = tmp_path / "live.db"
    db.write_text("db", encoding="utf-8")
    wal = Path(str(db) + ".wal")
    wal.write_text("active\n", encoding="utf-8")

    backup = QuasarBackup(tmp_path / "backups")
    backup.backup(db, version="v1")
    with pytest.raises(QuasarRestoreError, match="quiesce"):
        backup.restore("v1", db, overwrite=True, force=False, quiesce_sec=60.0)


def test_restore_force_allows_recent_wal(tmp_path: Path):
    db = tmp_path / "live.db"
    db.write_text("db", encoding="utf-8")
    wal = Path(str(db) + ".wal")
    wal.write_text("active\n", encoding="utf-8")

    backup = QuasarBackup(tmp_path / "backups")
    backup.backup(db, version="v1")
    backup.restore("v1", db, overwrite=True, force=True)
    assert db.exists()


def test_database_lock_exclusive(tmp_path: Path):
    db = tmp_path / "shard.db"
    db.write_text("", encoding="utf-8")
    lock1 = DatabaseLock(db)
    lock1.acquire()
    lock2 = DatabaseLock(db, timeout_sec=0.0)
    from quasar.errors import QuasarLockError

    with pytest.raises(QuasarLockError):
        lock2.acquire()
    lock1.release()
    lock2.acquire()
    lock2.release()


def test_replica_drift_detects_mismatch(tmp_path: Path, mock_client: AstralDBClient):
    master = tmp_path / "master.db"
    replica = tmp_path / "replica.db"
    master.write_text("aaa", encoding="utf-8")
    replica.write_text("bbb", encoding="utf-8")
    rep = QuasarReplica(ReplicaSet(master=master, replicas=[replica]), client=mock_client)
    report = QuasarDrift.check_replica_set(rep, label="mirror")
    assert not report.consistent
    assert report.outliers


def test_cluster_execute_merged(mock_client: AstralDBClient, shard_config: dict):
    cluster = QuasarCluster(shard_config, client=mock_client)
    merged = cluster.execute_merged("SELECT 1;")
    assert merged.shard_count == 3
    assert merged.all_ok


def test_cluster_drift_with_replicas(mock_client: AstralDBClient, tmp_path: Path):
    master = tmp_path / "m.db"
    slave = tmp_path / "s.db"
    master.write_text("x", encoding="utf-8")
    slave.write_text("y", encoding="utf-8")
    cfg = {
        "shards": [{"name": "s0", "database": str(master)}],
        "virtual_nodes": 8,
        "replicas": {"set0": {"master": str(master), "slaves": [str(slave)]}},
    }
    cluster = QuasarCluster(cfg, client=mock_client)
    reports = cluster.check_drift(use_hashes=False, check_replicas=True)
    replica_reports = [r for r in reports if r["method"].startswith("replica_hash")]
    assert replica_reports
    assert not replica_reports[0]["consistent"]
