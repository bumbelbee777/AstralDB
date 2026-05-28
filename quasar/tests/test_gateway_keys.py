from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from quasar.gateway_keys import (
    GatewayKeyStore,
    format_api_token,
    hash_gateway_secret,
    parse_api_token,
    resolve_gateway_credential,
)


def test_gateway_key_roundtrip(tmp_path: Path):
    path = tmp_path / "gateway_keys.json"
    store = GatewayKeyStore(path)
    token, key_id = store.create_key(description="test")
    assert token.startswith("qk_")
    kid, secret = parse_api_token(token)
    assert kid == key_id
    rec = store.verify_token(token)
    assert rec is not None
    assert rec.key_id == key_id
    assert not store.verify_token(token + "x")


def test_gateway_key_revoke(tmp_path: Path):
    path = tmp_path / "gateway_keys.json"
    store = GatewayKeyStore(path)
    token, key_id = store.create_key()
    assert store.verify_token(token) is not None
    store.revoke_key(key_id)
    assert store.verify_token(token) is None


def test_resolve_bearer_and_header(tmp_path: Path):
    path = tmp_path / "gateway_keys.json"
    store = GatewayKeyStore(path)
    token, _ = store.create_key()
    ok, kid = resolve_gateway_credential(
        headers={"Authorization": f"Bearer {token}"},
        query_args={},
        legacy_plaintext=None,
        key_store=store,
        allow_query_api_key=False,
        require_auth=True,
    )
    assert ok and kid
    ok2, _ = resolve_gateway_credential(
        headers={"X-Quasar-Key": token},
        query_args={},
        legacy_plaintext=None,
        key_store=store,
        allow_query_api_key=False,
        require_auth=True,
    )
    assert ok2


def test_query_api_key_disabled_by_default(tmp_path: Path):
    path = tmp_path / "gateway_keys.json"
    store = GatewayKeyStore(path)
    token, _ = store.create_key()
    ok, _ = resolve_gateway_credential(
        headers={},
        query_args={"api_key": token},
        legacy_plaintext=None,
        key_store=store,
        allow_query_api_key=False,
        require_auth=True,
    )
    assert not ok


def test_gateway_flask_hashed_key(tmp_path: Path, monkeypatch):
    pytest.importorskip("flask")
    from quasar.quasar import QuasarCluster, create_gateway_app

    keys_file = tmp_path / "keys.json"
    store = GatewayKeyStore(keys_file)
    token, _ = store.create_key()

    cfg = {
        "shards": [{"name": "s0", "database": str(tmp_path / "s0.db")}],
        "virtual_nodes": 8,
        "security": {"require_gateway_auth": True},
    }
    cluster = QuasarCluster(cfg, config_path=tmp_path / "cluster.json")
    monkeypatch.delenv("QUASAR_API_KEY", raising=False)
    app = create_gateway_app(cluster, key_store_path=keys_file)
    client = app.test_client()
    denied = client.get("/health")
    assert denied.status_code == 401
    allowed = client.get("/health", headers={"Authorization": f"Bearer {token}"})
    assert allowed.status_code == 200
