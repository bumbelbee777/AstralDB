from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import pytest

from quasar.errors import QuasarOverloadError
from quasar.quasar import QuasarCluster


class _AuthHandler(BaseHTTPRequestHandler):
    last_path = ""

    def do_GET(self):  # noqa: N802
        _AuthHandler.last_path = self.path
        if self.headers.get("Authorization") != "Bearer abc":
            self.send_response(401)
            self.end_headers()
            return
        body = json.dumps(
            [
                {"id": 7, "status": "ok", "amount": 50},
                {"id": 8, "status": "bad", "amount": 10},
            ]
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # noqa: A003
        return


def _serve_auth():
    server = HTTPServer(("127.0.0.1", 0), _AuthHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def test_fdw_stats_and_auth(mock_client, shard_config: dict, tmp_path: Path):
    cfg = dict(shard_config)
    cfg["fdw"] = {"enabled": True}
    cfg_path = tmp_path / "cluster.json"
    cfg_path.write_text(json.dumps(cfg), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    server = _serve_auth()
    base_url = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        cluster.execute(
            f"CREATE FOREIGN SOURCE ext TYPE http_json OPTIONS (url='{base_url}', auth_token='abc');"
        )
        cluster.execute("CREATE FOREIGN TABLE ext_orders SOURCE ext OBJECT orders;")
        rows = cluster.execute("SELECT id,amount FROM ext_orders WHERE amount>=20 AND status='ok' LIMIT 1;")
        payload = json.loads(rows[0].result.stdout)
        assert payload[0]["id"] == 7
        assert "fields=id%2Camount" in _AuthHandler.last_path
        assert "filter=amount%3E%3D20+AND+status%3Dok" in _AuthHandler.last_path
        stats = cluster.fdw.stats()
        assert stats["requests"] >= 1.0
        assert "ext" in stats["sources"]
    finally:
        server.shutdown()
        cluster.close()


def test_htap_lane_budget_rejection(mock_client, shard_config: dict, tmp_path: Path):
    cfg = dict(shard_config)
    cfg["htap"] = {"enabled": True, "olap_queue_soft_limit": 0.1}
    cfg_path = tmp_path / "cluster.json"
    cfg_path.write_text(json.dumps(cfg), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    cluster.pool_stats = lambda: {"queue_depth": 100000.0}  # type: ignore[assignment]
    with pytest.raises(QuasarOverloadError):
        cluster.execute("SELECT a.id FROM a JOIN b ON a.id=b.id;")
    cluster.close()


def test_pitr_manifest_and_restore_markers(mock_client, shard_config: dict, tmp_path: Path):
    cfg = dict(shard_config)
    cfg["backup_dir"] = str(tmp_path / "backups")
    cfg["pitr"] = {"enabled": True, "timeline_id": "main"}
    cfg_path = tmp_path / "cluster.json"
    cfg_path.write_text(json.dumps(cfg), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    for node in cluster.shard.nodes:
        cluster.client.query("SELECT 1;", database=node.database)
    versions = cluster.backup_all()
    node = cluster.shard.nodes[0]
    version = Path(str(versions[node.name])).name
    manifest = cluster.backup.describe_version(version)  # type: ignore[union-attr]
    assert manifest.timeline_id == "main"
    archived = cluster.pitr_archive_wal("mk1")
    assert archived
    restored = cluster.restore_to_marker(version, tmp_path / "restore.db", overwrite=True)
    assert restored.exists()
    cluster.close()


def test_upgrade_state_machine(mock_client, shard_config: dict, tmp_path: Path):
    cfg = dict(shard_config)
    cfg["upgrades"] = {"enabled": True, "canary_shards": 1}
    cfg["gsi"] = {"enabled": True, "state_file": str(tmp_path / "gsi" / "indexes.json")}
    cfg_path = tmp_path / "cluster.json"
    cfg_path.write_text(json.dumps(cfg), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    started = cluster.upgrade_start("2.0.0")
    assert started["stage"] == "preflight"
    for _ in range(8):
        state = cluster.upgrade_tick()
        if state["stage"] in ("completed", "rolled_back"):
            break
    assert state["stage"] in ("completed", "rolled_back")
    created = cluster.gsi_create(name="idx_users_email", table="users", column="email")
    assert created["name"] == "idx_users_email"
    listed = cluster.gsi_list()
    assert listed["count"] >= 1
    dropped = cluster.gsi_drop("idx_users_email")
    assert dropped["dropped"] == "idx_users_email"
    cluster.close()
