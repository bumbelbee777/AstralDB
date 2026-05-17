"""Tests for multi-master, failover, rebalance, cross-shard join."""

import json
from pathlib import Path

import pytest

from quasar.crossjoin import CrossJoinSpec, QuasarCrossShardJoin
from quasar.failover import FailoverTarget, QuasarFailover
from quasar.multimaster import MultiMasterGroup, QuasarMultiMaster
from quasar.quasar import QuasarCluster, QuasarShard, ShardNode
from quasar.rebalance import QuasarRebalance


@pytest.fixture
def cluster_config(tmp_path: Path) -> Path:
    shards = [
        {"name": "alpha", "database": str(tmp_path / "alpha.db")},
        {"name": "beta", "database": str(tmp_path / "beta.db")},
        {"name": "gamma", "database": str(tmp_path / "gamma.db")},
    ]
    cfg = {
        "shards": shards,
        "virtual_nodes": 64,
        "multi_master": {
            "alpha": {"writers": [shards[0]["database"]], "quorum": 1},
        },
        "failover": {
            "enabled": True,
            "auto_promote": True,
            "state_file": str(tmp_path / "failover.json"),
            "shards": {
                "alpha": {
                    "primary": shards[0]["database"],
                    "standbys": [str(tmp_path / "alpha_standby.db")],
                }
            },
        },
        "rebalance": {"shard_key_column": "id"},
    }
    path = tmp_path / "cluster.json"
    path.write_text(json.dumps(cfg), encoding="utf-8")
    (tmp_path / "alpha_standby.db").write_text("standby", encoding="utf-8")
    return path


def test_multimaster_write(mock_client, shard_config: dict):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    db = shard.nodes[0].database
    db.parent.mkdir(parents=True, exist_ok=True)
    group = MultiMasterGroup("alpha", [db, db], quorum=2)
    mm = QuasarMultiMaster(shard, {"alpha": group}, client=mock_client)
    result = mm.write("INSERT INTO t VALUES (1);", shard_key="user:1")
    assert result.quorum_met


def test_failover_state(tmp_path: Path, mock_client, shard_config: dict):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    primary = shard.nodes[0].database
    primary.parent.mkdir(parents=True, exist_ok=True)
    primary.write_text("primary", encoding="utf-8")
    standby = tmp_path / "standby.db"
    standby.write_text("standby", encoding="utf-8")
    fo = QuasarFailover(
        shard,
        {
            "alpha": FailoverTarget("alpha", primary, [standby]),
        },
        client=mock_client,
        state_file=tmp_path / "fo.json",
    )
    assert fo.active_database(shard.nodes[0]) == primary
    fo._state["active"]["alpha"] = str(standby)
    assert fo.active_database(shard.nodes[0]) == standby


def test_rebalance_plan(mock_client, shard_config: dict, tmp_path: Path):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    for n in shard.nodes:
        n.database.parent.mkdir(parents=True, exist_ok=True)
        n.database.write_text("db", encoding="utf-8")
    rb = QuasarRebalance(shard, client=mock_client, shard_key_column="id")
    plan = rb.plan(["alpha", "beta", "gamma", "delta"], work_dir=tmp_path / "work")
    assert "move_count" in plan.to_dict() or isinstance(plan.moves, list)


def test_cross_shard_join(mock_client, shard_config: dict):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    for n in shard.nodes:
        n.database.parent.mkdir(parents=True, exist_ok=True)
        n.database.write_text("db", encoding="utf-8")
    spec = CrossJoinSpec.from_dict(
        {
            "tables": [
                {"alias": "o", "sql": "SELECT * FROM orders", "key_columns": ["customer_id"]},
                {"alias": "c", "sql": "SELECT * FROM customers", "key_columns": ["id"]},
            ],
            "joins": [{"left": "o", "right": "c", "on": [["customer_id", "id"]]}],
        }
    )
    xj = QuasarCrossShardJoin(shard, client=mock_client)
    rows = xj.execute(spec)
    assert isinstance(rows, list)
