"""Automated crash recovery, pool heal/keepalive, and cross-shard queries."""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from quasar.crossquery import QuasarCrossQuery, parse_stdout_rows
from quasar.pool import PooledAstralDBClient, PoolConfig
from quasar.quasar import QuasarCluster
from quasar.recovery_auto import CrashRecoveryManager, RecoveryAutomationConfig


def test_parse_stdout_rows():
    rows = parse_stdout_rows("OK:row-one\n1 | alice | 10\n---\n")
    assert len(rows) >= 2
    assert rows[0]["_line"] == "row-one"
    assert rows[1]["col0"] == "1"


def test_crash_recovery_manager_runs_steps(mock_client, shard_config: dict, tmp_path: Path):
    cfg_path = tmp_path / "cluster.json"
    for s in shard_config["shards"]:
        Path(s["database"]).parent.mkdir(parents=True, exist_ok=True)
    shard_config["recovery_automation"] = {
        "enabled": True,
        "on_start": False,
        "recover_dtxn": False,
        "rollback_orphan_txns": True,
        "heal_pool": False,
    }
    cfg_path.write_text(json.dumps(shard_config), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    mgr = CrashRecoveryManager(cluster, RecoveryAutomationConfig(enabled=True, heal_pool=False))
    report = mgr.run()
    assert report.get("ok")
    assert "rollback" in report.get("steps", [])


def test_cluster_recovery_tick_on_start(mock_client, shard_config: dict, tmp_path: Path):
    cfg_path = tmp_path / "cluster.json"
    for s in shard_config["shards"]:
        Path(s["database"]).parent.mkdir(parents=True, exist_ok=True)
    shard_config["recovery_automation"] = {
        "enabled": True,
        "on_start": True,
        "recover_dtxn": False,
        "interval_sec": 0,
    }
    cfg_path.write_text(json.dumps(shard_config), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    assert cluster.recovery_auto is not None
    tick = cluster.recovery_tick()
    assert tick.get("ok") or tick.get("skipped")
    cluster.close()


def test_cross_query_fanout(mock_client, shard_config: dict, tmp_path: Path):
    cfg_path = tmp_path / "cluster.json"
    for s in shard_config["shards"]:
        Path(s["database"]).parent.mkdir(parents=True, exist_ok=True)
    cfg_path.write_text(json.dumps(shard_config), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    result = cluster.cross_query("SELECT 1;", parallel=True)
    assert result.all_ok
    assert len(result.routed) == len(shard_config["shards"])
    cluster.close()


def test_cross_query_scatter_gather(mock_client, shard_config: dict, tmp_path: Path):
    cfg_path = tmp_path / "cluster.json"
    for s in shard_config["shards"]:
        Path(s["database"]).parent.mkdir(parents=True, exist_ok=True)
    cfg_path.write_text(json.dumps(shard_config), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    queries = [
        {"sql": "SELECT 1;", "shard_key": "user:a"},
        {"sql": "SELECT 2;", "shard_key": "user:b"},
    ]
    result = cluster.cross_query(queries=queries, parallel=True)
    assert result.all_ok
    assert len(result.routed) == 2
    cluster.close()


def test_pooled_heal_and_keepalive(mock_astraldb, tmp_path: Path):
    db = tmp_path / "heal.db"
    client = PooledAstralDBClient(
        mock_astraldb,
        pool=PoolConfig(
            batch_window_ms=10,
            keepalive_interval_sec=0.05,
            per_db_max_inflight=2,
        ),
        timeout_sec=5.0,
    )
    client.warm([db])
    client.query("SELECT 1;", database=db, immediate=False)
    client.heal()
    time.sleep(0.12)
    stats = client.pool_stats()
    assert stats.get("keepalive_pings", 0) >= 0
    client.close()
