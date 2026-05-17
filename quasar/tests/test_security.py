"""Security policy, path containment, and gateway hardening tests."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from quasar.errors import QuasarSecurityError
from quasar.security import (
    SecurityPolicy,
    atomic_write_json,
    constant_time_equal,
    resolve_path_under_base,
    validate_shard_key,
    validate_shard_name,
    validate_sql,
    validate_sql_identifier,
)


def test_constant_time_equal():
    assert constant_time_equal("secret", "secret")
    assert not constant_time_equal("secret", "Secret")
    assert not constant_time_equal(None, "secret")
    assert not constant_time_equal("secret", None)


def test_validate_sql_rejects_null_and_oversize():
    policy = SecurityPolicy(max_sql_bytes=16)
    validate_sql("SELECT 1", policy)
    with pytest.raises(QuasarSecurityError, match="null"):
        validate_sql("SELECT\x001", policy)
    with pytest.raises(QuasarSecurityError, match="max size"):
        validate_sql("x" * 32, policy)


def test_validate_shard_key():
    validate_shard_key("user:42")
    with pytest.raises(QuasarSecurityError):
        validate_shard_key("")
    with pytest.raises(QuasarSecurityError):
        validate_shard_key("a\nb")


def test_validate_identifiers_and_shard_names():
    validate_sql_identifier("orders")
    validate_shard_name("shard0")
    with pytest.raises(QuasarSecurityError):
        validate_sql_identifier("1bad")
    with pytest.raises(QuasarSecurityError):
        validate_shard_name("../evil")


def test_resolve_path_blocks_traversal(tmp_path: Path):
    base = tmp_path / "cluster"
    base.mkdir()
    (base / "data").mkdir()
    safe = resolve_path_under_base(base, "data/shard0.db")
    assert safe == (base / "data" / "shard0.db").resolve()
    with pytest.raises(QuasarSecurityError, match="escapes"):
        resolve_path_under_base(base, "../../etc/passwd")


def test_atomic_write_json(tmp_path: Path):
    target = tmp_path / "state.json"
    atomic_write_json(target, {"ok": True})
    assert json.loads(target.read_text(encoding="utf-8")) == {"ok": True}


def test_config_rejects_traversal_in_shard_path(tmp_path: Path):
    from quasar.config import load_cluster_config

    cfg = tmp_path / "cluster.json"
    cfg.write_text(
        json.dumps(
            {
                "shards": [{"name": "s0", "database": "../../../outside.db"}],
                "virtual_nodes": 8,
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(Exception, match="escapes|Security|path"):
        load_cluster_config(cfg)


def test_gateway_requires_auth_when_configured(tmp_path: Path, monkeypatch):
    from quasar.quasar import QuasarCluster, create_gateway_app

    cfg = {
        "shards": [{"name": "s0", "database": str(tmp_path / "s0.db")}],
        "virtual_nodes": 8,
        "security": {"require_gateway_auth": True},
    }
    cluster = QuasarCluster(cfg, config_path=tmp_path / "cluster.json")
    monkeypatch.delenv("QUASAR_API_KEY", raising=False)
    with pytest.raises(ValueError, match="QUASAR_API_KEY"):
        create_gateway_app(cluster, api_key=None)


def test_gateway_constant_time_auth(tmp_path: Path):
    pytest.importorskip("flask")
    from quasar.quasar import QuasarCluster, create_gateway_app

    cfg = {
        "shards": [{"name": "s0", "database": str(tmp_path / "s0.db")}],
        "virtual_nodes": 8,
    }
    cluster = QuasarCluster(cfg, config_path=tmp_path / "cluster.json")
    app = create_gateway_app(cluster, api_key="correct-key")
    client = app.test_client()
    bad = client.post(
        "/query",
        json={"sql": "SELECT 1"},
        headers={"X-Quasar-Key": "wrong-key"},
    )
    assert bad.status_code == 401
    good = client.post(
        "/query",
        json={"sql": "SELECT 1"},
        headers={"X-Quasar-Key": "correct-key"},
    )
    assert good.status_code in (200, 500)  # mock CLI may error; auth passed
