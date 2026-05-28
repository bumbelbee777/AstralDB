"""Tests for AstralDB CLI client wrapper."""

from pathlib import Path

import pytest

from quasar.client import AstralDBClient, find_astraldb
from quasar.errors import QuasarSecurityError


def test_find_astraldb_explicit(mock_astraldb: Path):
    assert find_astraldb(mock_astraldb) == mock_astraldb.resolve()


def test_find_astraldb_missing(tmp_path: Path):
    with pytest.raises(QuasarSecurityError, match="not found"):
        find_astraldb(tmp_path / "nope.exe")


def test_client_version(mock_only_client: AstralDBClient):
    assert "mock" in mock_only_client.version().lower()


def test_client_query_creates_db(mock_only_client: AstralDBClient, tmp_path: Path):
    db = tmp_path / "test.db"
    result = mock_only_client.query("SELECT 1;", database=db)
    assert result.ok
    assert "OK:" in result.stdout
    assert db.exists()


def test_client_raises_on_failure(mock_astraldb: Path):
    client = AstralDBClient(executable=mock_astraldb)
    with pytest.raises(RuntimeError):
        client.run(["--unknown-flag-xyz"]).raise_on_error()
