"""Consensus quorum, strict 2PC, and automatic rebalance."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from quasar.consensus import ClusterConsensus, ConsensusConfig
from quasar.dtxn import CoordinatorRecord, CoordinatorLog, DistributedTransactionCoordinator, DistributedTxnConfig, TxnPhase
from quasar.quasar import QuasarCluster
from quasar.rebalance_auto import QuasarRebalanceAutomator, RebalanceAutomationConfig
from quasar.xtxn import TxnStatement


def test_consensus_propose_quorum(tmp_path: Path):
    cfg = ConsensusConfig(
        enabled=True,
        members=("a", "b", "c"),
        self_id="a",
        state_dir=str(tmp_path / "consensus"),
    )
    cons = ClusterConsensus(cfg)
    cons.elect_leader()
    vote = cons.propose("test_op", {"n": 1})
    assert vote["committed"]
    assert vote["votes"] >= 3


def test_strict_2pc_logs_decision_before_commit(mock_client, tmp_path: Path):
    shards = [
        {"name": "s0", "database": str(tmp_path / "s0.db")},
        {"name": "s1", "database": str(tmp_path / "s1.db")},
    ]
    wal = tmp_path / "dtxn.jsonl"
    cfg = {
        "shards": shards,
        "virtual_nodes": 8,
        "pool": {"enabled": False},
        "distributed_txn": {
            "enabled": True,
            "wal_file": str(wal),
            "participant_log_dir": str(tmp_path / "parts"),
            "recover_on_start": False,
            "strict_2pc": True,
            "require_consensus_for_commit": False,
        },
    }
    path = tmp_path / "cluster.json"
    path.write_text(json.dumps(cfg), encoding="utf-8")
    cluster = QuasarCluster(cfg, client=mock_client, config_path=path)
    assert cluster.dtxn is not None
    result = cluster.dtxn.run(
        [
            TxnStatement("INSERT INTO t VALUES (1);", "user:a"),
            TxnStatement("INSERT INTO t VALUES (2);", "user:b"),
        ]
    )
    assert result.committed and result.acid
    latest = cluster.dtxn.log.latest_by_xid()[result.xid]
    assert latest.phase == "committed"
    records = cluster.dtxn.log.load_all()
    phases = [r.phase for r in records if r.xid == result.xid]
    assert TxnPhase.DECISION_COMMITTED.value in phases
    assert phases.index(TxnPhase.DECISION_COMMITTED.value) < phases.index(TxnPhase.COMMITTED.value)
    cluster.close()


def test_recovery_completes_decision_committed(mock_client, tmp_path: Path):
    from quasar.quasar import QuasarShard

    shards = [
        {"name": "s0", "database": str(tmp_path / "s0.db")},
        {"name": "s1", "database": str(tmp_path / "s1.db")},
    ]
    for s in shards:
        Path(s["database"]).parent.mkdir(parents=True, exist_ok=True)
    shard = QuasarShard.from_config({"shards": shards, "virtual_nodes": 8}, client=mock_client)
    wal = tmp_path / "wal.jsonl"
    coord = DistributedTransactionCoordinator(
        shard,
        mock_client,
        wal_path=wal,
        participant_log_dir=tmp_path / "parts",
        config=DistributedTxnConfig(require_consensus_for_commit=False),
    )
    coord.log.append(
        CoordinatorRecord(
            xid="xid-commit",
            phase=TxnPhase.DECISION_COMMITTED.value,
            participants=["s0", "s1"],
            statements=[],
            ts=0,
        )
    )
    actions = coord.recover()
    assert any(a["action"] == "committed" for a in actions)


def test_rebalance_auto_tick_skipped_without_moves(mock_client, shard_config: dict, tmp_path: Path):
    cfg_path = tmp_path / "cluster.json"
    for s in shard_config["shards"]:
        Path(s["database"]).parent.mkdir(parents=True, exist_ok=True)
    shard_config["rebalance"] = {
        "shard_key_column": "id",
        "automation": {"enabled": True, "auto_apply": False},
    }
    cfg_path.write_text(json.dumps(shard_config), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    auto = QuasarRebalanceAutomator(
        cluster, RebalanceAutomationConfig(enabled=True, auto_apply=False)
    )
    report = auto.tick()
    assert report.get("action") == "none"
    cluster.close()


def test_dtxn_with_consensus(mock_client, tmp_path: Path):
    shards = [
        {"name": "s0", "database": str(tmp_path / "s0.db")},
        {"name": "s1", "database": str(tmp_path / "s1.db")},
    ]
    cfg = {
        "shards": shards,
        "virtual_nodes": 8,
        "pool": {"enabled": False},
        "consensus": {
            "enabled": True,
            "members": ["s0", "s1"],
            "self_id": "s0",
            "state_dir": str(tmp_path / "consensus"),
        },
        "distributed_txn": {
            "enabled": True,
            "wal_file": str(tmp_path / "dtxn.jsonl"),
            "participant_log_dir": str(tmp_path / "parts"),
            "recover_on_start": False,
            "require_consensus_for_commit": True,
        },
    }
    path = tmp_path / "cluster.json"
    path.write_text(json.dumps(cfg), encoding="utf-8")
    cluster = QuasarCluster(cfg, client=mock_client, config_path=path)
    assert cluster.consensus is not None
    assert cluster.consensus.is_leader()
    result = cluster.dtxn.run([TxnStatement("SELECT 1;", "user:a")])
    assert result.committed
    cluster.close()
