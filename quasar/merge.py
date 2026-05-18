"""Merge fan-out query results from multiple shards into one response."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from quasar.quasar import RoutedResult

_READ_PREFIXES = ("SELECT", "WITH", "SHOW", "EXPLAIN", "DESCRIBE", "PRAGMA")
_DML_PREFIXES = ("INSERT", "UPDATE", "DELETE", "CREATE", "ALTER", "DROP", "TRUNCATE", "REPLACE")


def looks_like_read_query(sql: str) -> bool:
    """Heuristic: safe to concatenate stdout from every shard."""
    stripped = sql.lstrip()
    if not stripped:
        return False
    head = stripped.split(None, 1)[0].upper()
    if head in _DML_PREFIXES:
        return False
    return head in _READ_PREFIXES or head.startswith("(") and "SELECT" in stripped.upper()[:80]


def merge_stdout(
    parts: List[str],
    *,
    dedupe_blank: bool = True,
) -> str:
    """Concatenate per-shard stdout, optionally skipping repeated blank lines."""
    if not parts:
        return ""
    if not dedupe_blank:
        return "".join(parts)
    out: List[str] = []
    prev_blank = False
    for chunk in parts:
        for line in chunk.splitlines(keepends=True):
            is_blank = line.strip() == ""
            if is_blank and prev_blank:
                continue
            out.append(line)
            prev_blank = is_blank
    return "".join(out)


@dataclass
class MergedFanoutResult:
    """Combined fan-out outcome for clients that want a single payload."""

    merged_stdout: str
    shard_count: int
    per_shard: List[Dict[str, Any]]
    all_ok: bool

    def to_dict(self) -> Dict[str, Any]:
        return {
            "merged_stdout": self.merged_stdout,
            "shard_count": self.shard_count,
            "all_ok": self.all_ok,
            "shards": self.per_shard,
        }


def merge_routed_results(
    results: List["RoutedResult"],
    *,
    sql: Optional[str] = None,
    dedupe_blank: bool = True,
) -> MergedFanoutResult:
    """Merge RoutedResult list; raises ValueError if sql looks like DML and len>1."""
    if sql is not None and len(results) > 1 and not looks_like_read_query(sql):
        raise ValueError(
            "cannot merge fan-out results for non-read SQL; omit merge or route with shard_key"
        )
    per_shard = [
        {
            "node": r.node,
            "database": str(r.database),
            "stdout": r.result.stdout,
            "stderr": r.result.stderr,
            "ok": r.result.ok,
            "returncode": r.result.returncode,
            "elapsed_ms": r.result.elapsed_ms,
        }
        for r in results
    ]
    merged = merge_stdout([r.result.stdout for r in results], dedupe_blank=dedupe_blank)
    return MergedFanoutResult(
        merged_stdout=merged,
        shard_count=len(results),
        per_shard=per_shard,
        all_ok=all(r.result.ok for r in results),
    )
