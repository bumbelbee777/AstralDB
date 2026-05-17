"""Tests for inventory and drift helpers."""

from pathlib import Path

from quasar.drift import QuasarDrift
from quasar.inventory import QuasarInventory, sha256_file
from quasar.quasar import QuasarShard, ShardNode


def test_inventory_totals(tmp_path: Path):
    db = tmp_path / "a.db"
    db.write_bytes(b"x" * 100)
    wal = Path(str(db) + ".wal")
    wal.write_bytes(b"y" * 10)
    inv = QuasarInventory([ShardNode("a", db)])
    report = inv.collect()
    assert report["total_db_bytes"] == 100
    assert report["total_wal_bytes"] == 10


def test_sha256_file(tmp_path: Path):
    p = tmp_path / "f.bin"
    p.write_bytes(b"abc")
    assert sha256_file(p) is not None
    assert len(sha256_file(p) or "") == 64


def test_drift_hash_detects_difference(mock_client, shard_config: dict, tmp_path: Path):
    shard = QuasarShard.from_config(shard_config, client=mock_client)
    for i, node in enumerate(shard.nodes):
        node.database.write_bytes(bytes([i + 1]) * 50)
    drift = QuasarDrift(shard)
    report = drift.check_hashes()
    assert not report.consistent
    assert len(report.outliers) >= 1
