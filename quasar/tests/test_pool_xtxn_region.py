"""Tests for pool, cross-shard transactions, and multi-region."""

import json
from pathlib import Path

import pytest

from quasar.pool import PooledAstralDBClient, PoolConfig
from quasar.region import QuasarMultiRegion
from quasar.xtxn import CrossShardTransaction, TxnStatement


def test_pooled_client_batches(mock_astraldb, tmp_path: Path):
    db = tmp_path / "pooled.db"
    client = PooledAstralDBClient(
        mock_astraldb,
        pool=PoolConfig(batch_window_ms=20, batch_max_statements=10),
        timeout_sec=5.0,
    )
    client.warm([db])
    r1 = client.query("SELECT 1;", database=db, immediate=False)
    r2 = client.query("SELECT 2;", database=db, immediate=False)
    assert r1.ok and r2.ok
    stats = client.pool_stats()
    assert stats["queries_submitted"] >= 2
    client.close()


def test_cross_shard_txn_commit(mock_client, shard_config: dict):
    shard_config_path = shard_config
    from quasar.quasar import QuasarShard

    shard = QuasarShard.from_config(shard_config, client=mock_client)
    for n in shard.nodes:
        n.database.parent.mkdir(parents=True, exist_ok=True)
        n.database.write_text("x", encoding="utf-8")
    txn = CrossShardTransaction(shard, client=mock_client)
    result = txn.run(
        [
            TxnStatement("INSERT INTO t VALUES (1);", "key-a"),
            TxnStatement("INSERT INTO t VALUES (2);", "key-b"),
        ]
    )
    assert result.committed
    assert len(result.participants) >= 1


def test_multi_region_write(mock_client, tmp_path: Path):
    regions = {
        "us": {
            "write_primary": True,
            "priority": 0,
            "shards": [{"name": "s0", "database": str(tmp_path / "us_s0.db")}],
        },
        "eu": {
            "write_primary": False,
            "priority": 1,
            "shards": [{"name": "s0", "database": str(tmp_path / "eu_s0.db")}],
        },
    }
    config = {"regions": regions, "virtual_nodes": 32}
    mr = QuasarMultiRegion.from_config(config, client=mock_client)
    for spec in mr.regions.values():
        for s in spec.shards:
            s.database.parent.mkdir(parents=True, exist_ok=True)
    wr = mr.write("INSERT INTO x VALUES (1);", shard_key="user:1")
    assert wr.region == "us"
    assert "eu" in wr.replicated_to
    mr.close()
