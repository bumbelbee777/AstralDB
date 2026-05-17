"""Parse and run Quasar-annotated SQL batch files."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from quasar.quasar import QuasarShard

SHARD_DIRECTIVE = re.compile(
    r"^\s*--\s*@(?:shard(?:-key)?|quasar:shard)\s+(.+?)\s*$",
    re.IGNORECASE,
)
BROADCAST_DIRECTIVE = re.compile(
    r"^\s*--\s*@(?:broadcast|all-shards|quasar:broadcast)\s*$",
    re.IGNORECASE,
)


@dataclass
class BatchStatement:
    sql: str
    shard_key: Optional[str] = None
    broadcast: bool = False
    line_no: int = 0


def parse_batch_file(path: Path) -> List[BatchStatement]:
    """Parse a .sql file with optional `-- @shard KEY` / `-- @broadcast` directives."""
    statements: List[BatchStatement] = []
    current_key: Optional[str] = None
    current_broadcast = False
    buffer: List[str] = []
    line_no = 0

    def flush(end_line: int) -> None:
        nonlocal buffer, current_key, current_broadcast
        sql = "\n".join(buffer).strip()
        buffer = []
        if not sql:
            return
        statements.append(
            BatchStatement(
                sql=sql,
                shard_key=None if current_broadcast else current_key,
                broadcast=current_broadcast,
                line_no=end_line,
            )
        )

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line_no += 1
        m_shard = SHARD_DIRECTIVE.match(raw_line)
        if m_shard:
            flush(line_no - 1)
            current_key = m_shard.group(1).strip()
            current_broadcast = False
            continue
        if BROADCAST_DIRECTIVE.match(raw_line):
            flush(line_no - 1)
            current_broadcast = True
            current_key = None
            continue
        if raw_line.strip().upper() == "GO":
            flush(line_no)
            current_key = None
            current_broadcast = False
            continue
        buffer.append(raw_line)

    flush(line_no)
    return statements


class QuasarSqlBatch:
    """Execute a directive-aware SQL batch against a shard group."""

    def __init__(self, shard: QuasarShard) -> None:
        self.shard = shard

    def run_file(self, path: Path) -> List[dict]:
        results: List[dict] = []
        for stmt in parse_batch_file(path):
            if stmt.broadcast:
                routed = self.shard.execute(stmt.sql, shard_key=None, parallel=True)
            elif stmt.shard_key:
                routed = self.shard.execute(stmt.sql, shard_key=stmt.shard_key)
            else:
                routed = self.shard.execute(stmt.sql, shard_key=None, parallel=False)
            results.append(
                {
                    "line": stmt.line_no,
                    "shard_key": stmt.shard_key,
                    "broadcast": stmt.broadcast,
                    "nodes": [
                        {
                            "node": r.node,
                            "ok": r.result.ok,
                            "elapsed_ms": r.result.elapsed_ms,
                        }
                        for r in routed
                    ],
                }
            )
        return results
