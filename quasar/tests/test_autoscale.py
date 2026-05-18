"""Autoscaling evaluation and scale-out apply."""

from __future__ import annotations

import json
from pathlib import Path

from quasar.autoscale import AutoscaleConfig, QuasarAutoscale
from quasar.client import QueryResult
from quasar.quasar import QuasarCluster


def test_autoscale_evaluate_scale_out(mock_client, shard_config: dict, tmp_path: Path):
    cfg_path = tmp_path / "cluster.json"
    for s in shard_config["shards"]:
        Path(s["database"]).parent.mkdir(parents=True, exist_ok=True)
    cfg_path.write_text(json.dumps(shard_config), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    mgr = QuasarAutoscale(
        cluster,
        AutoscaleConfig(
            enabled=True,
            max_shards=8,
            scale_out_mean_latency_ms=100,
            cooldown_sec=0,
        ),
    )
    for _ in range(5):
        cluster.monitor.record(
            QueryResult(stdout="", stderr="", returncode=0, elapsed_ms=500.0)
        )
    decision = mgr.evaluate()
    assert decision.action == "scale_out"
    assert "latency" in decision.reasons
    cluster.close()


def test_autoscale_apply_adds_shard(mock_client, shard_config: dict, tmp_path: Path):
    cfg_path = tmp_path / "cluster.json"
    data_dir = tmp_path / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    for s in shard_config["shards"]:
        p = data_dir / Path(s["database"]).name
        s["database"] = f"data/{p.name}"
        p.touch()
    shard_config["autoscaling"] = {
        "enabled": True,
        "max_shards": 6,
        "cooldown_sec": 0,
    }
    cfg_path.write_text(json.dumps(shard_config), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    mgr = QuasarAutoscale(cluster, AutoscaleConfig(enabled=True, max_shards=6, cooldown_sec=0))
    before = len(cluster.shard.nodes)
    result = mgr.apply_scale_out(count=1)
    assert result.get("added")
    cluster.reload_shards()
    assert len(cluster.shard.nodes) == before + 1
    raw = json.loads(cfg_path.read_text(encoding="utf-8"))
    assert len(raw["shards"]) == before + 1
    cluster.close()


def test_autoscale_tick_no_apply(mock_client, shard_config: dict, tmp_path: Path):
    cfg_path = tmp_path / "cluster.json"
    shard_config["autoscaling"] = {"enabled": True, "auto_apply": False, "cooldown_sec": 0}
    cfg_path.write_text(json.dumps(shard_config), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    report = cluster.autoscale_tick(apply=False)
    assert "decision" in report
    assert report.get("applied") is None
    cluster.close()
