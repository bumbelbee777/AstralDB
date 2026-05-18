"""Cross-platform path helper tests."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from quasar.config import load_cluster_config
from quasar.paths import (
    coerce_path,
    is_path_under_base,
    path_for_cli,
    path_for_config,
    path_key,
    resolve_path_under_base,
)
from quasar.security import QuasarSecurityError


def test_coerce_path_accepts_forward_slashes_on_windows():
    if os.name != "nt":
        pytest.skip("Windows-specific")
    p = coerce_path("data/shard0.db")
    assert p == Path("data/shard0.db")


def test_path_for_config_uses_forward_slashes(tmp_path: Path):
    db = tmp_path / "data" / "shard0.db"
    db.parent.mkdir(parents=True)
    text = path_for_config(db)
    assert "/" in text or text.endswith("shard0.db")
    assert "\\" not in text or os.name != "nt"


def test_path_key_dedupes_slash_styles(tmp_path: Path):
    db = tmp_path / "same.db"
    db.write_text("x", encoding="utf-8")
    a = path_key(db)
    b = path_key(str(db).replace("\\", "/") if "\\" in str(db) else str(db))
    assert a == b


def test_resolve_path_under_base_forward_slashes(tmp_path: Path):
    base = tmp_path / "cluster"
    base.mkdir()
    (base / "data").mkdir()
    resolved = resolve_path_under_base(base, "data/shard0.db")
    assert resolved == (base / "data" / "shard0.db").resolve()


def test_resolve_path_under_base_blocks_traversal(tmp_path: Path):
    base = tmp_path / "cluster"
    base.mkdir()
    with pytest.raises(QuasarSecurityError, match="escapes"):
        resolve_path_under_base(base, "../../outside.db")


def test_is_path_under_base_case_insensitive_on_windows(tmp_path: Path):
    if os.name != "nt":
        pytest.skip("Windows-specific")
    base = tmp_path / "Cluster"
    base.mkdir()
    child = base / "data" / "x.db"
    child.parent.mkdir(parents=True)
    assert is_path_under_base(child, base)


def test_load_cluster_config_normalizes_nested_paths(tmp_path: Path):
    import json

    cfg = {
        "shards": [{"name": "s0", "database": "data/s0.db"}],
        "distributed_txn": {
            "wal_file": ".quasar/dtxn/wal.jsonl",
            "participant_log_dir": ".quasar/dtxn/participants",
        },
        "financial": {
            "enabled": True,
            "journal_file": ".quasar/journal.jsonl",
        },
    }
    config_path = tmp_path / "cluster.json"
    (tmp_path / "data").mkdir()
    config_path.write_text(json.dumps(cfg), encoding="utf-8")
    loaded = load_cluster_config(config_path)
    assert loaded["shards"][0]["database"].endswith("data/s0.db")
    assert loaded["distributed_txn"]["wal_file"].endswith(".quasar/dtxn/wal.jsonl")
    assert Path(loaded["financial"]["journal_file"]).is_absolute()


def test_path_for_cli_returns_native_string(tmp_path: Path):
    db = tmp_path / "native.db"
    db.write_text("", encoding="utf-8")
    cli = path_for_cli(db)
    assert isinstance(cli, str)
    assert Path(cli) == db.resolve()
