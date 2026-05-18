"""Distributed ACID coordinator and improved failover."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from quasar.dtxn import CoordinatorLog, DistributedTransactionCoordinator, DistributedTxnConfig
from quasar.failover import FailoverPolicy, FailoverTarget, QuasarFailover
from quasar.quasar import QuasarCluster, QuasarShard
from quasar.xtxn import TxnStatement


@pytest.fixture
def dtxn_cluster(mock_client, tmp_path: Path) -> QuasarCluster:
    shards = [
        {"name": "s0", "database": str(tmp_path / "s0.db")},
        {"name": "s1", "database": str(tmp_path / "s1.db")},
    ]
    cfg = {
        "shards": shards,
        "virtual_nodes": 8,
        "distributed_txn": {
            "enabled": True,
            "wal_file": str(tmp_path / "dtxn.jsonl"),
            "recover_on_start": False,
        },
        "pool": {"enabled": False},
    }
    path = tmp_path / "cluster.json"
    path.write_text(json.dumps(cfg), encoding="utf-8")
    for s in shards:
        Path(s["database"]).parent.mkdir(parents=True, exist_ok=True)
    return QuasarCluster(cfg, client=mock_client, config_path=path)


def test_coordinator_commits_with_wal(dtxn_cluster: QuasarCluster):
    assert dtxn_cluster.dtxn is not None
    result = dtxn_cluster.dtxn.run(
        [
            TxnStatement("INSERT INTO t VALUES (1);", "user:a"),
            TxnStatement("INSERT INTO t VALUES (2);", "user:b"),
        ]
    )
    assert result.committed
    assert result.acid
    assert result.xid
    latest = dtxn_cluster.dtxn.log.latest_by_xid()
    assert latest[result.xid].phase == "committed"


def test_coordinator_recovery_aborts_in_doubt(dtxn_cluster: QuasarCluster, tmp_path: Path):
    wal = tmp_path / "recover.jsonl"
    shard = dtxn_cluster.shard
    coord = DistributedTransactionCoordinator(
        shard,
        dtxn_cluster.client,
        wal_path=wal,
        participant_log_dir=tmp_path / "parts",
    )
    coord.log.append(
        __import__("quasar.dtxn", fromlist=["CoordinatorRecord"]).CoordinatorRecord(
            xid="xid-stuck",
            phase="prepared",
            participants=["s0", "s1"],
            statements=[{"sql": "SELECT 1", "shard_key": "x"}],
            ts=0,
        )
    )
    actions = coord.recover()
    assert len(actions) == 1
    assert actions[0]["xid"] == "xid-stuck"


def test_failover_requires_consecutive_failures(mock_client, tmp_path: Path, shard_config: dict):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    primary = shard.nodes[0].database
    primary.parent.mkdir(parents=True, exist_ok=True)
    primary.write_text("primary", encoding="utf-8")
    standby = tmp_path / "standby.db"
    standby.write_text("standby", encoding="utf-8")

    calls = {"n": 0}

    def flaky_query(sql, **kwargs):
        calls["n"] += 1
        if calls["n"] <= 2 and "SELECT 1" in sql:
            raise RuntimeError("down")
        return mock_client.query(sql, **kwargs)

    client = mock_client
    client.query = flaky_query  # type: ignore[method-assign]

    fo = QuasarFailover(
        shard,
        {"alpha": FailoverTarget("alpha", primary, [standby])},
        client=client,
        state_file=tmp_path / "fo.json",
        policy=FailoverPolicy(failure_threshold=3, cooldown_after_promote_sec=0),
    )
    a1 = fo.tick()
    assert any(a.get("status") == "degraded" for a in a1)
    assert not any("promoted" in a for a in a1)


def test_failover_fail_back(mock_client, tmp_path: Path, shard_config: dict):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    primary = shard.nodes[0].database
    primary.parent.mkdir(parents=True, exist_ok=True)
    primary.write_text("primary", encoding="utf-8")
    standby = tmp_path / "standby.db"
    standby.write_text("standby", encoding="utf-8")
    fo = QuasarFailover(
        shard,
        {"alpha": FailoverTarget("alpha", primary, [standby])},
        client=mock_client,
        state_file=tmp_path / "fo.json",
        policy=FailoverPolicy(failure_threshold=1, cooldown_after_promote_sec=0),
    )
    fo._state["using_standby"]["alpha"] = True
    fo._state["active"]["alpha"] = str(standby)
    path = fo.fail_back("alpha")
    assert path == primary
