from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlparse

from quasar.pool import PoolConfig, PooledAstralDBClient
from quasar.quasar import QuasarCluster


class _Handler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if urlparse(self.path).path.rstrip("/") == "/orders":
            payload = [{"id": 1, "amount": 10}, {"id": 2, "amount": 20}]
        else:
            payload = {"rows": []}
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):  # noqa: A003
        return


def _serve():
    server = HTTPServer(("127.0.0.1", 0), _Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def test_fdw_create_and_select(mock_client, shard_config: dict, tmp_path: Path):
    cfg = dict(shard_config)
    cfg["fdw"] = {"enabled": True, "timeout_sec": 1.5}
    cfg_path = tmp_path / "cluster.json"
    cfg_path.write_text(json.dumps(cfg), encoding="utf-8")
    cluster = QuasarCluster.from_file(cfg_path, client=mock_client)
    server = _serve()
    base_url = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        cluster.execute(f"CREATE FOREIGN SOURCE ext TYPE http_json OPTIONS (url='{base_url}');")
        cluster.execute("CREATE FOREIGN TABLE ext_orders SOURCE ext OBJECT orders;")
        rows = cluster.execute("SELECT * FROM ext_orders LIMIT 1;")
        payload = json.loads(rows[0].result.stdout)
        assert rows[0].node == "__fdw__"
        assert len(payload) == 1
        assert payload[0]["id"] == 1
    finally:
        server.shutdown()
        cluster.close()


def test_pool_emits_queue_metrics(mock_astraldb: Path):
    client = PooledAstralDBClient(
        executable=mock_astraldb,
        timeout_sec=5.0,
        pool=PoolConfig(
            max_workers=2,
            max_queue=200,
            batch_max_statements=8,
            batch_window_ms=1.0,
            overload_soft_limit_ratio=0.75,
            overload_hard_limit_ratio=0.95,
        ),
    )
    db = mock_astraldb.parent / "pool-metrics.db"
    for i in range(20):
        client.query(f"INSERT INTO t VALUES ({i});", database=db)
    stats = client.pool_stats()
    client.close()
    assert stats["queue_wait_ms_mean"] >= 0.0
    assert stats["batch_elapsed_ms_mean"] >= 0.0
    assert "queue_depth" in stats
