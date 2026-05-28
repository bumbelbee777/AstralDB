from __future__ import annotations

import json
import time
from pathlib import Path

from quasar.quasar import QuasarCluster


def _write_cfg(tmp_path: Path, cfg: dict) -> Path:
    p = tmp_path / "cluster.json"
    p.write_text(json.dumps(cfg), encoding="utf-8")
    return p


def test_cdc_record_poll_publish(mock_client, shard_config: dict, tmp_path: Path):
    cfg = dict(shard_config)
    cfg["cdc"] = {
        "enabled": True,
        "timeline_id": "main",
        "checkpoint_file": str(tmp_path / "cdc" / "checkpoints.json"),
        "sink_file": str(tmp_path / "cdc" / "events.jsonl"),
        "dlq_file": str(tmp_path / "cdc" / "dlq.jsonl"),
    }
    cfg_path = _write_cfg(tmp_path, cfg)
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    cluster.execute("INSERT INTO t VALUES (1);", shard_key="k1")
    polled = cluster.cdc_poll()
    assert polled["count"] >= 1
    published = cluster.cdc_publish("cg1")
    assert published["published"] >= 1
    published_b = cluster.cdc_publish_batched("cg2", chunk_size=1)
    assert published_b["published"] >= 1
    cluster.close()


def test_distributed_join_execute(mock_client, shard_config: dict, tmp_path: Path):
    cfg = dict(shard_config)
    cfg["distributed_join"] = {"enabled": True, "broadcast_threshold": 100}
    cfg_path = _write_cfg(tmp_path, cfg)
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    spec = {
        "tables": [
            {"alias": "o", "sql": "SELECT * FROM orders", "key_columns": ["customer_id"]},
            {"alias": "c", "sql": "SELECT * FROM customers", "key_columns": ["id"]},
        ],
        "joins": [{"left": "o", "right": "c", "on": [["customer_id", "id"]]}],
        "limit": 10,
    }
    spec_path = tmp_path / "join.json"
    spec_path.write_text(json.dumps(spec), encoding="utf-8")
    out = cluster.distributed_join_execute(spec_path)
    assert out["plan"]["strategy"] in ("broadcast_hash_join", "repartition_hash_join")
    assert out["count"] >= 1
    stream = cluster.distributed_join_execute_stream(spec_path, chunk_size=1)
    assert stream["count"] >= 1
    assert len(stream["chunks"]) >= 1
    cluster.close()


def test_split_merge_serverless_edge(mock_client, shard_config: dict, tmp_path: Path):
    cfg = dict(shard_config)
    cfg["split_merge"] = {"enabled": True, "journal_file": str(tmp_path / "sm" / "journal.json")}
    cfg["serverless"] = {"enabled": True, "state_file": str(tmp_path / "srv" / "lease.json"), "idle_sec": 0.01}
    cfg["edge"] = {"enabled": True, "registry_file": str(tmp_path / "edge" / "registry.json")}
    cfg["cdc"] = {
        "enabled": True,
        "checkpoint_file": str(tmp_path / "cdc" / "checkpoints.json"),
        "sink_file": str(tmp_path / "cdc" / "events.jsonl"),
        "dlq_file": str(tmp_path / "cdc" / "dlq.jsonl"),
    }
    cfg_path = _write_cfg(tmp_path, cfg)
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)

    split = cluster.split_shard("alpha", split_key="m")
    merge = cluster.merge_shards(["alpha", "beta"], target="gamma")
    split_c = cluster.split_shard_chunked("alpha", split_key="m", chunk_size=10, total_rows=25)
    merge_c = cluster.merge_shards_chunked(["alpha", "beta"], target="gamma", chunk_size=10, total_rows=25)
    tick = cluster.split_merge_tick()
    assert split["planned"] and merge["planned"]
    assert split_c["chunks"] == 3 and merge_c["chunks"] == 3
    assert tick["journal_records"] >= 2
    assert tick["binary_records"] >= 6

    lease = cluster.serverless_acquire("worker-1")
    assert lease["active"] is True
    time.sleep(0.02)
    tick_srv = cluster.serverless_tick()
    assert tick_srv["active"] is False

    reg = cluster.edge_register("edge-1", "eu-west")
    assert reg["node_id"] == "edge-1"
    cluster.execute("UPDATE t SET v=2 WHERE id=1;", shard_key="k1")
    sync = cluster.edge_sync("edge-1", after_commit=0, limit=100)
    assert sync["applied_events"] >= 1
    cluster.close()
