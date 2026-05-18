"""Tests for cluster config loading and validation."""

import json
from pathlib import Path

import pytest

from quasar.config import ConfigError, load_cluster_config, validate_cluster_config


def test_validate_rejects_empty_shards():
    with pytest.raises(ConfigError):
        validate_cluster_config({"shards": []})


def test_validate_rejects_duplicate_names():
    cfg = {
        "shards": [
            {"name": "a", "database": "x.db"},
            {"name": "a", "database": "y.db"},
        ]
    }
    with pytest.raises(ConfigError):
        validate_cluster_config(cfg)


def test_load_resolves_relative_paths(tmp_path: Path):
    cfg = {
        "shards": [{"name": "s0", "database": "data/s0.db"}],
        "backup_dir": "backups",
    }
    config_path = tmp_path / "cluster.json"
    (tmp_path / "data").mkdir()
    config_path.write_text(json.dumps(cfg), encoding="utf-8")
    loaded = load_cluster_config(config_path)
    assert loaded["shards"][0]["database"].endswith("data/s0.db")
    assert loaded["backup_dir"].endswith("backups")


def test_backup_prune_dry_run(tmp_path: Path):
    from quasar.quasar import QuasarBackup

    backup = QuasarBackup(tmp_path / "bk")
    db = tmp_path / "x.db"
    for i in range(5):
        db.write_text(f"v{i}", encoding="utf-8")
        backup.backup(db, version=f"v{i}")
    removed = backup.prune(keep_last=2, dry_run=True)
    assert len(removed) == 3
