"""Tests for Quasar orchestration (shard, replica, backup, migration)."""

import json
from pathlib import Path

import pytest

from quasar.client import AstralDBClient
from quasar.quasar import (
    QuasarBackup,
    QuasarMigration,
    QuasarMonitor,
    QuasarReplica,
    QuasarShard,
    ReplicaSet,
    ShardNode,
)


def test_shard_routes_by_key(mock_client: AstralDBClient, shard_config: dict, tmp_path: Path):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    key = "customer-42"
    node = shard.node_for_key(key)
    results = shard.execute("INSERT INTO t VALUES (1);", shard_key=key)
    assert len(results) == 1
    assert results[0].node == node.name
    assert results[0].database.exists()


def test_shard_broadcast(mock_client: AstralDBClient, shard_config: dict):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    results = shard.execute("SELECT 1;", parallel=False)
    assert len(results) == 3
    assert {r.node for r in results} == {"alpha", "beta", "gamma"}


def test_replica_write_all(mock_client: AstralDBClient, tmp_path: Path):
    master = tmp_path / "master.db"
    rep1 = tmp_path / "rep1.db"
    replica = QuasarReplica(ReplicaSet(master=master, replicas=[rep1]), client=mock_client)
    replica.write("CREATE TABLE x (id INT);")
    assert master.exists()
    assert rep1.exists()


def test_backup_and_restore(tmp_path: Path):
    db = tmp_path / "live.db"
    db.write_text("payload", encoding="utf-8")
    wal = Path(str(db) + ".wal")
    wal.write_text("wal-lines\n", encoding="utf-8")

    backup = QuasarBackup(tmp_path / "backups")
    dest = backup.backup(db, version="v1")
    assert (dest / "manifest.json").exists()
    assert (dest / "live.db").read_text(encoding="utf-8") == "payload"

    target = tmp_path / "restored.db"
    backup.restore("v1", target)
    assert target.read_text(encoding="utf-8") == "payload"
    assert Path(str(target) + ".wal").exists()


def test_incremental_backup_skips_empty_wal(tmp_path: Path):
    db = tmp_path / "x.db"
    db.write_text("x", encoding="utf-8")
    backup = QuasarBackup(tmp_path / "bk")
    assert backup.incremental_backup(db) is None


def test_incremental_backup_copies_wal(tmp_path: Path):
    db = tmp_path / "x.db"
    db.write_text("x", encoding="utf-8")
    wal = Path(str(db) + ".wal")
    wal.write_text("data\n", encoding="utf-8")
    backup = QuasarBackup(tmp_path / "bk")
    dest = backup.incremental_backup(db, version="inc1")
    assert dest is not None
    assert (dest / wal.name).exists()


def test_migration_bundle(mock_only_client: AstralDBClient, tmp_path: Path):
    source = tmp_path / "src.db"
    source.write_text("old", encoding="utf-8")
    target = tmp_path / "dst.db"
    mig = QuasarMigration(source, target, client=mock_only_client)
    bundle = mig.migrate_via_bundle(work_dir=tmp_path / "work")
    assert bundle.exists()
    assert target.read_text(encoding="utf-8") == "imported"


def test_migration_bundle_integration(astraldb_client: AstralDBClient, tmp_path: Path):
    source = tmp_path / "src.db"
    target = tmp_path / "dst.db"
    astraldb_client.query(
        "CREATE TABLE mig_t (id INT); INSERT INTO mig_t VALUES (1);",
        database=source,
        immediate=True,
    )
    mig = QuasarMigration(source, target, client=astraldb_client)
    bundle = mig.migrate_via_bundle(work_dir=tmp_path / "work")
    assert bundle.exists()
    assert target.is_file()
    out = astraldb_client.query("SELECT id FROM mig_t LIMIT 1;", database=target, immediate=True)
    assert out.ok


def test_monitor_percentiles():
    from quasar.client import QueryResult

    mon = QuasarMonitor()
    for ms in (1.0, 2.0, 3.0, 100.0):
        mon.record(QueryResult("", "", 0, ms))
    snap = mon.snapshot()
    assert snap["queries"] == 4.0
    assert snap["p50_latency_ms"] >= 1.0
    assert snap["p99_latency_ms"] >= snap["p50_latency_ms"]


def test_shard_health(mock_client: AstralDBClient, shard_config: dict):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    shard.execute("SELECT 1;", shard_key="k")
    health = shard.health()
    assert all(health.values())
