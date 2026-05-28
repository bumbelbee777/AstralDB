from __future__ import annotations

import json
import sqlite3
from pathlib import Path

import pytest

from quasar.migrate_util import QuasarMigrate, detect_migration_mode, is_sqlite_file, sqlite_to_bundle


def test_detect_sqlite_vs_astral(tmp_path: Path, mock_only_client):
    sqlite_path = tmp_path / "app.sqlite"
    conn = sqlite3.connect(str(sqlite_path))
    conn.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)")
    conn.execute("INSERT INTO t VALUES (1, 'ada')")
    conn.commit()
    conn.close()
    assert is_sqlite_file(sqlite_path)
    assert detect_migration_mode(sqlite_path) == "sqlite"

    astral_path = tmp_path / "native.db"
    mock_only_client.query("CREATE TABLE x (id INT);", database=astral_path, immediate=True)
    assert not is_sqlite_file(astral_path)
    assert detect_migration_mode(astral_path) == "astral"


def test_sqlite_to_bundle_shape(tmp_path: Path):
    sqlite_path = tmp_path / "users.sqlite"
    conn = sqlite3.connect(str(sqlite_path))
    conn.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT NOT NULL)")
    conn.execute("INSERT INTO users VALUES (1, 'a@example.com')")
    conn.commit()
    conn.close()
    bundle = sqlite_to_bundle(sqlite_path)
    assert bundle["kind"] == "astraldb.database.v1"
    assert "users" in bundle["tables"]
    assert bundle["tables"]["users"]["rows"][0]["email"] == "a@example.com"


def test_migrate_sqlite_into_astraldb(astraldb_client, tmp_path: Path):
    sqlite_path = tmp_path / "legacy.sqlite"
    conn = sqlite3.connect(str(sqlite_path))
    conn.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, body TEXT)")
    conn.execute("INSERT INTO messages VALUES (1, 'hello')")
    conn.execute("INSERT INTO messages VALUES (2, 'world')")
    conn.commit()
    conn.close()

    target = tmp_path / "migrated.db"
    mig = QuasarMigrate(client=astraldb_client)
    plan = mig.plan(sqlite_path, target)
    assert plan.mode == "sqlite"
    assert plan.row_counts["messages"] == 2

    result = mig.run(sqlite_path, target, mode="sqlite", work_dir=tmp_path / "work")
    assert target.is_file()
    verify = astraldb_client.query("SELECT id FROM messages LIMIT 1;", database=target, immediate=True)
    assert verify.ok


def test_migrate_sql_script(astraldb_client, tmp_path: Path):
    script = tmp_path / "schema.sql"
    script.write_text(
        "CREATE TABLE IF NOT EXISTS accounts (id INT, balance INT);\n"
        "INSERT INTO accounts VALUES (1, 100);\n",
        encoding="utf-8",
    )
    target = tmp_path / "from_script.db"
    out = QuasarMigrate(client=astraldb_client).run(script, target, mode="script")
    assert out["statements_executed"] >= 2
    res = astraldb_client.query("SELECT balance FROM accounts WHERE id = 1;", database=target, immediate=True)
    assert res.ok


def test_migrate_external_uri_dry_run():
    plan = QuasarMigrate().plan(
        "postgresql://user:pass@localhost:5432/legacy",
        Path("data/out.db"),
        mode="external",
    )
    assert plan.mode == "external"
    assert any("SQLAlchemy" in n for n in plan.notes)


def test_migrate_dry_run(tmp_path: Path):
    sqlite_path = tmp_path / "x.sqlite"
    conn = sqlite3.connect(str(sqlite_path))
    conn.execute("CREATE TABLE t (id INT)")
    conn.commit()
    conn.close()
    plan = QuasarMigrate().plan(sqlite_path, tmp_path / "out.db", mode="sqlite")
    assert plan.tables == ["t"]
