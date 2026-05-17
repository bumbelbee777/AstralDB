"""Cross-shard JOIN: fan-out, collect bundles/rows, hash-join in Python."""

from __future__ import annotations

import json
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from quasar.client import AstralDBClient
from quasar.quasar import QuasarShard
from quasar.rebalance import _load_bundle_tables
from quasar.security import SecurityPolicy, load_json_config_file, validate_sql_identifier


@dataclass
class JoinTableSpec:
    """One side of a cross-shard join."""

    alias: str
    sql: str
    key_columns: List[str]
    shard_key: Optional[str] = None

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "JoinTableSpec":
        alias = validate_sql_identifier(str(data["alias"]), label="join alias")
        keys = list(data.get("key_columns", data.get("keys", ["id"])))
        for col in keys:
            validate_sql_identifier(str(col), label="key column")
        return cls(
            alias=alias,
            sql=data["sql"],
            key_columns=keys,
            shard_key=data.get("shard_key"),
        )


@dataclass
class JoinOnSpec:
    left: str
    right: str
    columns: List[Tuple[str, str]]

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "JoinOnSpec":
        cols = data.get("on") or data.get("columns") or []
        parsed: List[Tuple[str, str]] = []
        for item in cols:
            if isinstance(item, (list, tuple)) and len(item) == 2:
                parsed.append((str(item[0]), str(item[1])))
        return cls(left=data["left"], right=data["right"], columns=parsed)


@dataclass
class CrossJoinSpec:
    tables: List[JoinTableSpec]
    joins: List[JoinOnSpec]
    limit: Optional[int] = None

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "CrossJoinSpec":
        return cls(
            tables=[JoinTableSpec.from_dict(t) for t in data["tables"]],
            joins=[JoinOnSpec.from_dict(j) for j in data.get("joins", [])],
            limit=data.get("limit"),
        )

    @classmethod
    def from_file(cls, path: Path, *, policy: Optional[SecurityPolicy] = None) -> "CrossJoinSpec":
        return cls.from_dict(load_json_config_file(path, policy))


def _row_key(row: Dict[str, Any], columns: Sequence[str]) -> Tuple[Any, ...]:
    return tuple(row.get(c) for c in columns)


def _prefix_row(alias: str, row: Dict[str, Any]) -> Dict[str, Any]:
    return {f"{alias}.{k}": v for k, v in row.items()}


class QuasarCrossShardJoin:
    """
    Execute per-shard SELECTs (via export bundle + table extraction), then join in memory.

    Join spec example (JSON):
    {
      "tables": [
        {"alias": "o", "sql": "SELECT * FROM orders", "key_columns": ["customer_id"]},
        {"alias": "c", "sql": "SELECT * FROM customers", "key_columns": ["id"], "shard_key": "tenant:1"}
      ],
      "joins": [{"left": "o", "right": "c", "on": [["customer_id", "id"]]}],
      "limit": 1000
    }
    """

    def __init__(self, shard: QuasarShard, client: Optional[AstralDBClient] = None) -> None:
        self.shard = shard
        self.client = client or shard.client

    def _fetch_table_rows(
        self,
        spec: JoinTableSpec,
        *,
        work_dir: Path,
    ) -> List[Dict[str, Any]]:
        nodes = self.shard.nodes
        if spec.shard_key:
            nodes = [self.shard.node_for_key(spec.shard_key)]

        all_rows: List[Dict[str, Any]] = []
        for node in nodes:
            bundle = work_dir / f"{spec.alias}_{node.name}.json"
            self.client.export_bundle(bundle, database=node.database, fmt="json")
            tables = _load_bundle_tables(bundle)
            table_name = _infer_table_from_sql(spec.sql)
            if table_name and table_name in tables:
                rows = tables[table_name]
            else:
                rows = []
                for rows_list in tables.values():
                    rows.extend(rows_list)
            for row in rows:
                tagged = dict(row)
                tagged["_shard"] = node.name
                all_rows.append(tagged)
        return all_rows

    def execute(self, spec: CrossJoinSpec) -> List[Dict[str, Any]]:
        work = Path(tempfile.mkdtemp(prefix="quasar_xjoin_"))
        datasets: Dict[str, List[Dict[str, Any]]] = {}
        for table in spec.tables:
            datasets[table.alias] = self._fetch_table_rows(table, work_dir=work)

        if not spec.joins:
            out = datasets[spec.tables[0].alias]
            if spec.limit:
                out = out[: spec.limit]
            return out

        result = datasets[spec.joins[0].left]
        for join in spec.joins:
            right_rows = datasets[join.right]
            left_cols = [c[0] for c in join.columns]
            right_cols = [c[1] for c in join.columns]
            right_index: Dict[Tuple[Any, ...], List[Dict[str, Any]]] = {}
            for row in right_rows:
                key = _row_key(row, right_cols)
                right_index.setdefault(key, []).append(row)

            merged: List[Dict[str, Any]] = []
            for left in result:
                key = _row_key(left, left_cols)
                for right in right_index.get(key, []):
                    combined: Dict[str, Any] = {}
                    combined.update(_prefix_row(join.left, left))
                    combined.update(_prefix_row(join.right, right))
                    merged.append(combined)
            result = merged

        if spec.limit:
            result = result[: spec.limit]
        return result


def _infer_table_from_sql(sql: str) -> Optional[str]:
    parts = sql.strip().split()
    for i, p in enumerate(parts):
        if p.upper() == "FROM" and i + 1 < len(parts):
            return parts[i + 1].strip(";").strip("(").lower()
    return None
