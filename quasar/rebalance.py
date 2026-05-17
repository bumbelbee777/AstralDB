"""Automatic shard rebalancing when the hash ring changes."""

from __future__ import annotations

import json
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

from quasar.client import AstralDBClient
from quasar.ring import ConsistentHashRing
from quasar.quasar import QuasarShard
from quasar.security import sql_literal_safe, validate_sql_identifier

PathLike = str | Path


@dataclass
class RebalanceMove:
    shard_key: str
    table: str
    from_shard: str
    to_shard: str
    row: Dict[str, Any]

    def to_dict(self) -> Dict[str, Any]:
        return {
            "shard_key": self.shard_key,
            "table": self.table,
            "from_shard": self.from_shard,
            "to_shard": self.to_shard,
            "row": self.row,
        }


@dataclass
class RebalancePlan:
    shard_key_column: str
    moves: List[RebalanceMove] = field(default_factory=list)
    summary: Dict[str, int] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "shard_key_column": self.shard_key_column,
            "move_count": len(self.moves),
            "summary": self.summary,
            "moves": [m.to_dict() for m in self.moves[:500]],
            "truncated": len(self.moves) > 500,
        }


def _load_bundle_tables(bundle_path: Path) -> Dict[str, List[Dict[str, Any]]]:
    data = json.loads(bundle_path.read_text(encoding="utf-8"))
    tables = data.get("tables", {})
    out: Dict[str, List[Dict[str, Any]]] = {}
    for name, wrap in tables.items():
        rows = wrap.get("rows", []) if isinstance(wrap, dict) else []
        parsed: List[Dict[str, Any]] = []
        for row in rows:
            if isinstance(row, dict):
                parsed.append(row)
        out[name] = parsed
    return out


class QuasarRebalance:
    """
    Plan and apply row moves when consistent-hash ownership changes.

    Scans exported bundle JSON per source shard and queues rows whose
    shard_key_column hashes to a different shard on the new ring.
    """

    def __init__(
        self,
        shard: QuasarShard,
        client: Optional[AstralDBClient] = None,
        *,
        shard_key_column: str = "id",
        virtual_nodes: int = 128,
    ) -> None:
        self.shard = shard
        self.client = client or shard.client
        self.shard_key_column = validate_sql_identifier(shard_key_column, label="shard_key_column")
        self.virtual_nodes = virtual_nodes

    def plan(
        self,
        new_shard_names: Sequence[str],
        *,
        tables: Optional[List[str]] = None,
        work_dir: Optional[PathLike] = None,
    ) -> RebalancePlan:
        old_ring = self.shard.ring
        new_ring = ConsistentHashRing(list(new_shard_names), virtual_nodes=self.virtual_nodes)
        old_nodes = {n.name for n in self.shard.nodes}
        plan = RebalancePlan(shard_key_column=self.shard_key_column)

        work = Path(work_dir) if work_dir else Path(tempfile.mkdtemp(prefix="quasar_rebalance_"))
        work.mkdir(parents=True, exist_ok=True)

        by_name = {n.name: n for n in self.shard.nodes}
        for node in self.shard.nodes:
            bundle = work / f"{node.name}.bundle.json"
            self.client.export_bundle(bundle, database=node.database, fmt="json")
            table_rows = _load_bundle_tables(bundle)
            for table, rows in table_rows.items():
                if tables is not None and table not in tables:
                    continue
                for row in rows:
                    if self.shard_key_column not in row:
                        continue
                    key = str(row[self.shard_key_column])
                    old_owner = old_ring.node_for_key(key)
                    new_owner = new_ring.node_for_key(key)
                    if old_owner == new_owner:
                        continue
                    if old_owner not in old_nodes:
                        continue
                    plan.moves.append(
                        RebalanceMove(
                            shard_key=key,
                            table=table,
                            from_shard=old_owner,
                            to_shard=new_owner,
                            row=row,
                        )
                    )
                    plan.summary[f"{old_owner}->{new_owner}"] = (
                        plan.summary.get(f"{old_owner}->{new_owner}", 0) + 1
                    )
        return plan

    def apply(
        self,
        plan: RebalancePlan,
        *,
        delete_from_source: bool = False,
        batch_size: int = 100,
    ) -> Dict[str, int]:
        """Apply planned moves via INSERT on target shards (optional DELETE on source)."""
        by_name = {n.name: n for n in self.shard.nodes}
        applied = 0
        errors = 0

        buckets: Dict[tuple[str, str, str], List[RebalanceMove]] = {}
        for move in plan.moves:
            key = (move.to_shard, move.table, move.from_shard)
            buckets.setdefault(key, []).append(move)

        for (to_shard, table, from_shard), moves in buckets.items():
            target_db = by_name[to_shard].database
            source_db = by_name[from_shard].database
            table_id = validate_sql_identifier(table, label="table")
            col_id = validate_sql_identifier(plan.shard_key_column, label="shard_key_column")
            for i in range(0, len(moves), batch_size):
                chunk = moves[i : i + batch_size]
                inserts = []
                for move in chunk:
                    cols = list(move.row.keys())
                    for c in cols:
                        validate_sql_identifier(c, label="column")
                    vals = ", ".join(sql_literal_safe(move.row[c]) for c in cols)
                    col_list = ", ".join(cols)
                    inserts.append(f"INSERT INTO {table_id} ({col_list}) VALUES ({vals});")
                sql = "\n".join(inserts)
                try:
                    self.client.query(sql, database=target_db)
                    applied += len(chunk)
                except Exception:
                    errors += len(chunk)
                    continue
                if delete_from_source:
                    deletes = []
                    for move in chunk:
                        pk = sql_literal_safe(move.row.get(plan.shard_key_column))
                        deletes.append(f"DELETE FROM {table_id} WHERE {col_id} = {pk};")
                    try:
                        self.client.query("\n".join(deletes), database=source_db)
                    except Exception:
                        pass
        return {"applied": applied, "errors": errors}

    def auto_rebalance(
        self,
        new_shard_names: Sequence[str],
        *,
        apply: bool = False,
        **kwargs: Any,
    ) -> Dict[str, Any]:
        plan = self.plan(new_shard_names, **kwargs)
        result: Dict[str, Any] = {"plan": plan.to_dict()}
        if apply and plan.moves:
            result["apply"] = self.apply(plan, **kwargs)
        return result
