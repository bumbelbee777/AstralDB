"""Multi-master write groups: replicate writes across peer databases."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

from quasar.client import AstralDBClient, QueryResult
from quasar.quasar import QuasarShard, ShardNode


@dataclass
class MultiMasterGroup:
    """Multiple writable databases for the same logical shard."""

    shard_name: str
    writers: List[Path]
    quorum: int = 1

    def __post_init__(self) -> None:
        if self.quorum < 1:
            raise ValueError("quorum must be >= 1")
        if self.quorum > len(self.writers):
            self.quorum = len(self.writers)


@dataclass
class MultiMasterResult:
    shard: str
    results: List[QueryResult]
    quorum_met: bool


class QuasarMultiMaster:
    """Write SQL to every master in a shard group; succeed if quorum acks."""

    def __init__(
        self,
        shard: QuasarShard,
        groups: Dict[str, MultiMasterGroup],
        client: Optional[AstralDBClient] = None,
    ) -> None:
        self.shard = shard
        self.groups = groups
        self.client = client or shard.client

    def _group_for_key(self, shard_key: str) -> tuple[ShardNode, Optional[MultiMasterGroup]]:
        node = self.shard.node_for_key(shard_key)
        return node, self.groups.get(node.name)

    def write(self, sql: str, *, shard_key: str) -> MultiMasterResult:
        node, group = self._group_for_key(shard_key)
        if group is None:
            result = self.client.query(sql, database=node.database)
            return MultiMasterResult(shard=node.name, results=[result], quorum_met=True)

        results: List[QueryResult] = []
        with ThreadPoolExecutor(max_workers=len(group.writers)) as pool:
            futures = [pool.submit(self.client.query, sql, database=db) for db in group.writers]
            for fut in as_completed(futures):
                results.append(fut.result())

        ok = sum(1 for r in results if r.ok)
        quorum_met = ok >= group.quorum
        if not quorum_met:
            raise RuntimeError(
                f"multi-master quorum not met for {node.name}: {ok}/{group.quorum} succeeded"
            )
        return MultiMasterResult(shard=node.name, results=results, quorum_met=quorum_met)

    def read(self, sql: str, *, shard_key: str) -> QueryResult:
        """Read from first healthy writer (fallback order)."""
        node, group = self._group_for_key(shard_key)
        candidates = group.writers if group else [node.database]
        last_error: Optional[Exception] = None
        for db in candidates:
            try:
                return self.client.query(sql, database=db)
            except Exception as exc:  # noqa: BLE001
                last_error = exc
        raise RuntimeError(f"all multi-master readers failed for {node.name}") from last_error
