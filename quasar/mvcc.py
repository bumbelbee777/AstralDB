"""AstralDB snapshot semantics for consistent reads under concurrent writers."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any, Dict, Optional

from quasar.merge import looks_like_read_query

# AstralDB: BEGIN copies the main DB file to .txn.snap; ROLLBACK restores it.
# Reads inside BEGIN see a stable snapshot for that subprocess session.


@dataclass(frozen=True)
class MvccConfig:
    """How Quasar uses AstralDB file snapshots for routed reads."""

    snapshot_reads: bool = True
    """Wrap read SQL in BEGIN … COMMIT so each subprocess sees one snapshot."""

    read_immediate: bool = True
    """Route reads around the write batch queue (still uses snapshot when enabled)."""

    temporal_as_of: Optional[str] = None
    """If set, append FOR SYSTEM TIME AS OF to SELECT bodies (epoch or literal)."""

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]]) -> "MvccConfig":
        if not raw:
            return cls()
        as_of = raw.get("temporal_as_of")
        return cls(
            snapshot_reads=bool(raw.get("snapshot_reads", True)),
            read_immediate=bool(raw.get("read_immediate", True)),
            temporal_as_of=str(as_of) if as_of else None,
        )


def is_write_sql(sql: str) -> bool:
    return not looks_like_read_query(sql)


def _strip_trailing_semicolon(sql: str) -> str:
    return sql.strip().rstrip(";")


def wrap_snapshot_transaction(sql: str) -> str:
    """
    Run SQL inside AstralDB's snapshot transaction (whole-file copy on BEGIN).

    One subprocess, one consistent view of the database file for the statement(s).
    """
    body = _strip_trailing_semicolon(sql)
    return f"BEGIN;\n{body};\nCOMMIT;"


def wrap_savepoint(sql: str, name: str = "quasar_sp") -> str:
    """SAVEPOINT wrapper for recoverable sub-steps inside a larger transaction."""
    body = _strip_trailing_semicolon(sql)
    safe = re.sub(r"[^A-Za-z0-9_]", "_", name)[:32] or "quasar_sp"
    return f"SAVEPOINT {safe};\n{body};"


def rollback_savepoint_sql(name: str = "quasar_sp") -> str:
    safe = re.sub(r"[^A-Za-z0-9_]", "_", name)[:32] or "quasar_sp"
    return f"ROLLBACK TO SAVEPOINT {safe};"


def apply_temporal_as_of(sql: str, as_of: str) -> str:
    """Append FOR SYSTEM TIME AS OF to simple SELECT statements."""
    body = _strip_trailing_semicolon(sql)
    upper = body.upper()
    if not upper.startswith("SELECT"):
        return sql
    if " FOR SYSTEM TIME " in upper:
        return sql
    literal = as_of if as_of.startswith("'") else f"'{as_of}'"
    return f"{body} FOR SYSTEM TIME AS OF {literal};"


def prepare_read_sql(sql: str, mvcc: MvccConfig) -> str:
    """Apply MVCC / temporal options for a read statement."""
    out = sql
    if mvcc.temporal_as_of:
        out = apply_temporal_as_of(out, mvcc.temporal_as_of)
    if mvcc.snapshot_reads:
        out = wrap_snapshot_transaction(out)
    return out
