"""Shared fixtures for Quasar unit tests."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

# Allow `import quasar` from repo root without install.
_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from quasar.client import AstralDBClient  # noqa: E402


def create_mock_astraldb(tmp_path: Path) -> Path:
    """Build a minimal AstralDB CLI stub executable under *tmp_path*."""
    script = tmp_path / "mock_astraldb.py"
    script.write_text(
        r'''
import json
import sys
from pathlib import Path

def main():
    args = sys.argv[1:]
    if "-v" in args or "--version" in args:
        print("AstralDB mock 0.0-test")
        return
    db = None
    i = 0
    while i < len(args):
        if args[i] in ("--database",) and i + 1 < len(args):
            db = Path(args[i + 1])
            i += 2
            continue
        if args[i] in ("-q", "--query") and i + 1 < len(args):
            sql = args[i + 1]
            if db:
                db.parent.mkdir(parents=True, exist_ok=True)
                if not db.exists():
                    db.write_text("", encoding="utf-8")
                wal = Path(str(db) + ".wal")
                wal.write_text("touched\n", encoding="utf-8")
            if "BEGIN" in sql.upper():
                print("OK:BATCH")
            else:
                print(f"OK:{sql[:40]}")
            return
        if args[i] in ("-s",) and i + 1 < len(args):
            if db:
                db.parent.mkdir(parents=True, exist_ok=True)
                if not db.exists():
                    db.write_text("", encoding="utf-8")
            print(f"SCRIPT:{Path(args[i + 1]).name}")
            return
        if args[i] == "--export-bundle" and i + 1 < len(args):
            out = Path(args[i + 1])
            bundle = {
                "bundleVersion": 1.0,
                "kind": "astraldb.database.v1",
                "tables": {
                    "orders": {
                        "schema": [],
                        "rows": [
                            {"id": 1, "customer_id": 10, "amount": 5},
                            {"id": 2, "customer_id": 20, "amount": 9},
                        ],
                    },
                    "customers": {
                        "schema": [],
                        "rows": [
                            {"id": 10, "name": "alice"},
                            {"id": 20, "name": "bob"},
                        ],
                    },
                },
            }
            out.write_text(json.dumps(bundle), encoding="utf-8")
            print(f"Exported to {out}")
            return
        if args[i] in ("-cc", "--compile") and i + 2 < len(args):
            out = Path(args[i + 2])
            out.write_bytes(b"MOCK_ABC")
            print(f"Compiled to {out}")
            return
        if args[i] == "--import-bundle" and i + 1 < len(args):
            if db:
                db.parent.mkdir(parents=True, exist_ok=True)
                db.write_text("imported", encoding="utf-8")
            print("Imported")
            return
        i += 1
    print("mock astraldb", file=sys.stderr)
    sys.exit(2)

if __name__ == "__main__":
    main()
''',
        encoding="utf-8",
    )
    if os.name == "nt":
        exe = tmp_path / "mock_astraldb.cmd"
        exe.write_text(f'@python "{script}" %*\n', encoding="utf-8")
        return exe
    exe = tmp_path / "mock_astraldb"
    exe.write_text(f'#!/bin/sh\nexec python3 "{script}" "$@"\n', encoding="utf-8")
    exe.chmod(0o755)
    return exe


@pytest.fixture
def mock_astraldb(tmp_path: Path) -> Path:
    return create_mock_astraldb(tmp_path)


@pytest.fixture(scope="session")
def astraldb_bin() -> Path | None:
    """Release binary under bin/ (preferred for integration tests)."""
    exe = _REPO_ROOT / "bin" / "astraldb.exe"
    if exe.is_file():
        return exe
    alt = _REPO_ROOT / "bin" / "astraldb"
    return alt if alt.is_file() else None


@pytest.fixture
def mock_client(mock_astraldb: Path) -> AstralDBClient:
    """Fast stub CLI for unit tests (default suite)."""
    return AstralDBClient(executable=mock_astraldb, timeout_sec=10.0)


@pytest.fixture
def mock_only_client(mock_astraldb: Path) -> AstralDBClient:
    """Alias for mock_client (tests that assert mock-specific stdout)."""
    return AstralDBClient(executable=mock_astraldb, timeout_sec=10.0)


@pytest.fixture
def astraldb_client(astraldb_bin: Path | None, mock_astraldb: Path) -> AstralDBClient:
    """Integration client: prefers bin/astraldb.exe, falls back to mock if missing."""
    force_mock = os.environ.get("QUASAR_TEST_MOCK", "").strip().lower() in ("1", "true", "yes")
    if not force_mock and astraldb_bin is not None:
        return AstralDBClient(executable=astraldb_bin, timeout_sec=180.0)
    return AstralDBClient(executable=mock_astraldb, timeout_sec=10.0)


@pytest.fixture
def shard_config(tmp_path: Path) -> dict:
    shards = []
    for name in ("alpha", "beta", "gamma"):
        db = tmp_path / f"{name}.db"
        shards.append({"name": name, "database": str(db)})
    return {"shards": shards, "virtual_nodes": 64}
