"""Tests for annotated SQL batch parsing."""

from pathlib import Path

from quasar.sqlbatch import parse_batch_file


def test_parse_shard_and_broadcast(tmp_path: Path):
    script = tmp_path / "batch.sql"
    script.write_text(
        """-- @broadcast
CREATE TABLE t (id INT);
GO
-- @shard user:1
INSERT INTO t VALUES (1);
""",
        encoding="utf-8",
    )
    stmts = parse_batch_file(script)
    assert len(stmts) == 2
    assert stmts[0].broadcast is True
    assert stmts[1].shard_key == "user:1"


def test_parse_quasar_alias(tmp_path: Path):
    script = tmp_path / "b.sql"
    script.write_text("-- @quasar:shard order:99\nSELECT 1;\n", encoding="utf-8")
    stmts = parse_batch_file(script)
    assert stmts[0].shard_key == "order:99"
