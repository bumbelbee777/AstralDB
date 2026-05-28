"""Global secondary index orchestration for Quasar shards."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional


@dataclass
class GlobalSecondaryIndex:
    name: str
    table: str
    column: str
    include_columns: List[str]
    per_shard: Dict[str, Dict[str, Any]]

    def to_dict(self) -> Dict[str, Any]:
        return {
            "name": self.name,
            "table": self.table,
            "column": self.column,
            "include_columns": list(self.include_columns),
            "per_shard": dict(self.per_shard),
        }


class GlobalSecondaryIndexManager:
    def __init__(self, cluster: Any, *, state_file: Path) -> None:
        self.cluster = cluster
        self.state_file = Path(state_file)
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        self._indexes: Dict[str, GlobalSecondaryIndex] = {}
        self._load()

    def _load(self) -> None:
        if not self.state_file.exists():
            return
        raw = json.loads(self.state_file.read_text(encoding="utf-8"))
        for row in raw.get("indexes", []):
            spec = GlobalSecondaryIndex(
                name=str(row["name"]),
                table=str(row["table"]),
                column=str(row["column"]),
                include_columns=[str(c) for c in row.get("include_columns", [])],
                per_shard={str(k): dict(v) for k, v in row.get("per_shard", {}).items()},
            )
            self._indexes[spec.name] = spec

    def _persist(self) -> None:
        payload = {"indexes": [idx.to_dict() for idx in self._indexes.values()]}
        self.state_file.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    def create(
        self,
        *,
        name: str,
        table: str,
        column: str,
        include_columns: Optional[List[str]] = None,
    ) -> Dict[str, Any]:
        include_columns = include_columns or []
        per_shard: Dict[str, Dict[str, Any]] = {}
        include_sql = f" ({', '.join(include_columns)})" if include_columns else ""
        sql = f"CREATE INDEX IF NOT EXISTS {name} ON {table} ({column}){include_sql};"
        for node in self.cluster.shard.nodes:
            result = self.cluster.client.query(sql, database=node.database)
            per_shard[node.name] = {"database": str(node.database), "ok": bool(result.ok)}
        spec = GlobalSecondaryIndex(
            name=name,
            table=table,
            column=column,
            include_columns=include_columns,
            per_shard=per_shard,
        )
        self._indexes[name] = spec
        self._persist()
        return spec.to_dict()

    def drop(self, name: str) -> Dict[str, Any]:
        spec = self._indexes.get(name)
        if spec is None:
            raise KeyError(f"gsi not found: {name}")
        sql = f"DROP INDEX IF EXISTS {name};"
        for node in self.cluster.shard.nodes:
            self.cluster.client.query(sql, database=node.database)
        del self._indexes[name]
        self._persist()
        return {"dropped": name}

    def list(self) -> Dict[str, Any]:
        return {"count": len(self._indexes), "indexes": [x.to_dict() for x in self._indexes.values()]}

