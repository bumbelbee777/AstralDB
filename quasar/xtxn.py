"""Cross-shard transactions (2PC-style BEGIN / COMMIT / ROLLBACK across shards)."""

from __future__ import annotations

import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

from quasar.client import AstralDBClient, QueryResult
from quasar.quasar import QuasarShard, ShardNode


@dataclass
class TxnStatement:
    sql: str
    shard_key: str


@dataclass
class CrossShardTxnResult:
    committed: bool
    participants: List[str]
    statements: int
    errors: List[str] = field(default_factory=list)
    xid: Optional[str] = None
    acid: bool = False

    def to_dict(self) -> Dict[str, Any]:
        out = {
            "committed": self.committed,
            "participants": self.participants,
            "statements": self.statements,
            "errors": self.errors,
        }
        if self.xid:
            out["xid"] = self.xid
        if self.acid:
            out["acid"] = True
        return out


class CrossShardTransaction:
    """
    Interactive distributed transaction across AstralDB shard files.

    Prefer ``DistributedTransactionCoordinator`` (``distributed_txn`` in config) for
    coordinator 2PC with a durable WAL. This class remains for manual begin/execute/commit
    flows when the coordinator is disabled.
    """

    def __init__(
        self,
        shard: QuasarShard,
        client: Optional[AstralDBClient] = None,
        *,
        participant_names: Optional[Set[str]] = None,
    ) -> None:
        self.shard = shard
        self.client = client or shard.client
        self._participant_names = participant_names
        self._participants: List[ShardNode] = []
        self._open = False
        self._lock = threading.Lock()

    def _resolve_participants(self, keys: Optional[Set[str]] = None) -> List[ShardNode]:
        if self._participant_names:
            by_name = {n.name: n for n in self.shard.nodes}
            return [by_name[n] for n in self._participant_names if n in by_name]
        if keys:
            seen: Dict[str, ShardNode] = {}
            for key in keys:
                node = self.shard.node_for_key(key)
                seen[node.name] = node
            return list(seen.values())
        return list(self.shard.nodes)

    def _run_on_all(self, sql: str, nodes: List[ShardNode]) -> List[tuple[ShardNode, QueryResult]]:
        results: List[tuple[ShardNode, QueryResult]] = []
        with ThreadPoolExecutor(max_workers=len(nodes)) as pool:
            futures = {
                pool.submit(self.client.query, sql, database=n.database, immediate=True): n
                for n in nodes
            }
            for fut in as_completed(futures):
                n = futures[fut]
                results.append((n, fut.result()))
        return results

    def begin(self, *, shard_keys: Optional[List[str]] = None) -> List[str]:
        keys_set = set(shard_keys) if shard_keys else None
        self._participants = self._resolve_participants(keys_set)
        self._run_on_all("BEGIN;", self._participants)
        self._open = True
        return [n.name for n in self._participants]

    def execute(self, sql: str, *, shard_key: str) -> QueryResult:
        if not self._open:
            raise RuntimeError("transaction not open; call begin() first")
        node = self.shard.node_for_key(shard_key)
        if node.name not in {p.name for p in self._participants}:
            raise RuntimeError(f"shard {node.name} is not a transaction participant")
        return self.client.query(sql, database=node.database, immediate=True)

    def commit(self) -> CrossShardTxnResult:
        if not self._open:
            raise RuntimeError("transaction not open")
        errors: List[str] = []
        try:
            commit_outcomes = self._run_on_all("COMMIT;", self._participants)
            for node, result in commit_outcomes:
                if not result.ok:
                    errors.append(f"{node.name}: commit failed (rc={result.returncode})")
        except Exception as exc:
            errors.append(str(exc))
        if errors:
            self.rollback()
            self._open = False
            return CrossShardTxnResult(
                committed=False,
                participants=[n.name for n in self._participants],
                statements=0,
                errors=errors,
            )
        self._open = False
        return CrossShardTxnResult(
            committed=True,
            participants=[n.name for n in self._participants],
            statements=0,
            errors=[],
        )

    def rollback(self) -> None:
        nodes = self._participants or list(self.shard.nodes)
        for node in nodes:
            try:
                self.client.query("ROLLBACK;", database=node.database, immediate=True)
            except RuntimeError:
                pass
        self._open = False

    def run(self, statements: List[TxnStatement]) -> CrossShardTxnResult:
        """One-shot: begin → statements → commit or rollback."""
        keys = [s.shard_key for s in statements]
        participants = self.begin(shard_keys=keys)
        errors: List[str] = []
        count = 0
        try:
            for stmt in statements:
                self.execute(stmt.sql, shard_key=stmt.shard_key)
                count += 1
            result = self.commit()
            result.statements = count
            return result
        except Exception as exc:
            errors.append(str(exc))
            self.rollback()
            return CrossShardTxnResult(
                committed=False,
                participants=participants,
                statements=count,
                errors=errors,
            )

    def __enter__(self) -> "CrossShardTransaction":
        self.begin()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if exc_type is not None:
            self.rollback()
        else:
            self.commit()
