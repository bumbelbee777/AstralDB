from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from quasar.quasar import QuasarCluster


def _cluster(mock_client, shard_config: dict, tmp_path: Path) -> QuasarCluster:
    cfg = dict(shard_config)
    cfg["matview"] = {
        "enabled": True,
        "state_file": str(tmp_path / "matview" / "catalog.json"),
        "default_interval_sec": 0.01,
        "mutation_debounce_sec": 0.0,
        "max_failures": 2,
        "circuit_cooldown_sec": 60,
    }
    cfg_path = tmp_path / "cluster.json"
    cfg_path.write_text(json.dumps(cfg), encoding="utf-8")
    return QuasarCluster.from_file(cfg_path, client=mock_client)


def test_matview_ddl_create_and_refresh(mock_client, shard_config: dict, tmp_path: Path):
    cluster = _cluster(mock_client, shard_config, tmp_path)
    out = cluster.execute(
        "CREATE MATERIALIZED VIEW mv_orders STORAGE mv_orders_store "
        "AS SELECT id FROM orders REFRESH EVERY 1 SECONDS;"
    )
    payload = json.loads(out[0].result.stdout)
    assert payload["name"] == "mv_orders"
    listed = cluster.matview_list()
    assert listed["count"] == 1
    refreshed = cluster.execute("REFRESH MATERIALIZED VIEW mv_orders;")
    refresh_payload = json.loads(refreshed[0].result.stdout)
    assert refresh_payload["name"] == "mv_orders"
    stats = cluster.matview.stats()
    assert stats["refreshes"] >= 2.0
    cluster.close()


def test_matview_interval_tick(mock_client, shard_config: dict, tmp_path: Path):
    cluster = _cluster(mock_client, shard_config, tmp_path)
    cluster.matview_create(
        name="mv_tick",
        query_sql="SELECT id FROM t",
        storage_table="mv_tick_store",
        refresh_mode="interval",
        interval_sec=0.01,
    )
    time.sleep(0.02)
    tick = cluster.matview_tick()
    assert "mv_tick" in tick["refreshed"] or "mv_tick" in tick["due"]
    cluster.close()


def test_matview_on_mutation_autorefresh(mock_client, shard_config: dict, tmp_path: Path):
    cluster = _cluster(mock_client, shard_config, tmp_path)
    cluster.matview_create(
        name="mv_mut",
        query_sql="SELECT id FROM orders",
        storage_table="mv_mut_store",
        refresh_mode="on_mutation",
        source_tables=["orders"],
    )
    before = cluster.matview.stats()["refreshes"]
    cluster.execute("INSERT INTO orders VALUES (1);", shard_key="k1")
    after = cluster.matview.stats()["refreshes"]
    assert after > before
    cluster.close()


def test_matview_circuit_breaker(mock_client, shard_config: dict, tmp_path: Path):
    cluster = _cluster(mock_client, shard_config, tmp_path)
    cluster.matview_create(
        name="mv_fail",
        query_sql="SELECT id FROM t",
        storage_table="mv_fail_store",
        refresh_mode="manual",
    )

    original_query = cluster.client.query

    def _fail_query(sql, database=None, **kwargs):
        if "SELECT" in sql.upper():
            raise RuntimeError("simulated select failure")
        return original_query(sql, database=database, **kwargs)

    cluster.client.query = _fail_query  # type: ignore[method-assign]
    for _ in range(2):
        try:
            cluster.matview_refresh("mv_fail")
        except RuntimeError:
            pass
    with pytest.raises(RuntimeError, match="circuit open"):
        cluster.matview_refresh("mv_fail")
    cluster.close()
