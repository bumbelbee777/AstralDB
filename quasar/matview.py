"""Materialized view catalog, refresh orchestration, and autorefresh scheduling."""

from __future__ import annotations

import json
import re
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Set, Tuple

from quasar.crossquery import parse_stdout_rows


@dataclass
class MaterializedViewSpec:
    name: str
    query_sql: str
    storage_table: str
    refresh_mode: str
    interval_sec: float
    source_tables: List[str]
    enabled: bool = True
    per_shard: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    last_refresh_at: float = 0.0
    last_refresh_ms: float = 0.0
    failures: int = 0
    circuit_open_until: float = 0.0
    rows_last_refresh: int = 0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "name": self.name,
            "query_sql": self.query_sql,
            "storage_table": self.storage_table,
            "refresh_mode": self.refresh_mode,
            "interval_sec": self.interval_sec,
            "source_tables": list(self.source_tables),
            "enabled": self.enabled,
            "per_shard": dict(self.per_shard),
            "last_refresh_at": self.last_refresh_at,
            "last_refresh_ms": self.last_refresh_ms,
            "failures": self.failures,
            "circuit_open_until": self.circuit_open_until,
            "rows_last_refresh": self.rows_last_refresh,
        }


class MaterializedViewManager:
    """Per-shard MV refresh via DELETE + SELECT fan-out + row INSERT."""

    CREATE_RE = re.compile(
        r"^\s*CREATE\s+MATERIALIZED\s+VIEW\s+([A-Za-z_][A-Za-z0-9_]*)\s+"
        r"(?:STORAGE\s+([A-Za-z_][A-Za-z0-9_]*)\s+)?"
        r"AS\s+(.*)\s*;?\s*$",
        re.IGNORECASE | re.DOTALL,
    )
    REFRESH_TAIL_RE = re.compile(
        r"\s+REFRESH\s+(EVERY\s+([0-9]+(?:\.[0-9]+)?)\s+SECONDS?|ON\s+MUTATION(?:\s+OF\s+([A-Za-z0-9_,\s]+))?|MANUAL)\s*$",
        re.IGNORECASE,
    )
    REFRESH_RE = re.compile(
        r"^\s*REFRESH\s+MATERIALIZED\s+VIEW\s+([A-Za-z_][A-Za-z0-9_]*)\s*;?\s*$",
        re.IGNORECASE,
    )
    DROP_RE = re.compile(
        r"^\s*DROP\s+MATERIALIZED\s+VIEW\s+([A-Za-z_][A-Za-z0-9_]*)\s*;?\s*$",
        re.IGNORECASE,
    )
    ALTER_ENABLE_RE = re.compile(
        r"^\s*ALTER\s+MATERIALIZED\s+VIEW\s+([A-Za-z_][A-Za-z0-9_]*)\s+(ENABLE|DISABLE)\s*;?\s*$",
        re.IGNORECASE,
    )

    def __init__(self, cluster: Any, section: Optional[Dict[str, Any]] = None) -> None:
        from pathlib import Path

        section = section or {}
        self.cluster = cluster
        self.enabled = bool(section.get("enabled", True))
        self.state_file = Path(section.get("state_file", ".quasar/matview/catalog.json"))
        self.default_interval_sec = float(section.get("default_interval_sec", 60.0))
        self.max_failures = max(1, int(section.get("max_failures", 5)))
        self.circuit_cooldown_sec = float(section.get("circuit_cooldown_sec", 30.0))
        self.mutation_debounce_sec = float(section.get("mutation_debounce_sec", 2.0))
        self.on_mutation_enabled = bool(section.get("on_mutation_enabled", True))
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        self._views: Dict[str, MaterializedViewSpec] = {}
        self._pending_mutation: Dict[str, float] = {}
        self._stats: Dict[str, float] = {
            "refreshes": 0.0,
            "errors": 0.0,
            "skipped_circuit": 0.0,
            "skipped_debounce": 0.0,
            "latency_ms_total": 0.0,
        }
        self._load()

    def _load(self) -> None:
        if not self.state_file.exists():
            return
        raw = json.loads(self.state_file.read_text(encoding="utf-8"))
        for row in raw.get("views", []):
            spec = MaterializedViewSpec(
                name=str(row["name"]),
                query_sql=str(row["query_sql"]),
                storage_table=str(row["storage_table"]),
                refresh_mode=str(row.get("refresh_mode", "interval")),
                interval_sec=float(row.get("interval_sec", self.default_interval_sec)),
                source_tables=[str(t) for t in row.get("source_tables", [])],
                enabled=bool(row.get("enabled", True)),
                per_shard={str(k): dict(v) for k, v in row.get("per_shard", {}).items()},
                last_refresh_at=float(row.get("last_refresh_at", 0.0)),
                last_refresh_ms=float(row.get("last_refresh_ms", 0.0)),
                failures=int(row.get("failures", 0)),
                circuit_open_until=float(row.get("circuit_open_until", 0.0)),
                rows_last_refresh=int(row.get("rows_last_refresh", 0)),
            )
            self._views[spec.name] = spec

    def _persist(self) -> None:
        payload = {"views": [v.to_dict() for v in self._views.values()]}
        self.state_file.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    @staticmethod
    def _parse_source_tables(raw: Optional[str]) -> List[str]:
        if not raw:
            return []
        return [t.strip().lower() for t in raw.split(",") if t.strip()]

    @staticmethod
    def _infer_source_tables(query_sql: str) -> List[str]:
        found = re.findall(r"\bFROM\s+([A-Za-z_][A-Za-z0-9_]*)", query_sql, flags=re.IGNORECASE)
        return sorted({t.lower() for t in found})

    def try_apply_ddl(self, sql: str) -> Optional[Dict[str, Any]]:
        if not self.enabled:
            return None
        create = self.CREATE_RE.match(sql)
        if create:
            name, storage, body = create.groups()
            refresh_mode = "interval"
            interval_sec = self.default_interval_sec
            source_tables: List[str] = []
            query = body.strip().rstrip(";")
            tail = self.REFRESH_TAIL_RE.search(query)
            if tail:
                query = query[: tail.start()].strip()
                clause = tail.group(1).upper()
                if clause.startswith("EVERY"):
                    interval_sec = max(1.0, float(tail.group(2)))
                elif "ON MUTATION" in clause:
                    refresh_mode = "on_mutation"
                    source_tables = self._parse_source_tables(tail.group(3))
                else:
                    refresh_mode = "manual"
            if not source_tables:
                source_tables = self._infer_source_tables(query)
            storage_table = storage or f"mv_{name}"
            return self.create(
                name=name,
                query_sql=query,
                storage_table=storage_table,
                refresh_mode=refresh_mode,
                interval_sec=interval_sec,
                source_tables=source_tables,
                refresh_now=True,
            )
        refresh = self.REFRESH_RE.match(sql)
        if refresh:
            return self.refresh(refresh.group(1), reason="ddl")
        drop = self.DROP_RE.match(sql)
        if drop:
            return self.drop(drop.group(1))
        alter = self.ALTER_ENABLE_RE.match(sql)
        if alter:
            return self.set_enabled(alter.group(1), enabled=alter.group(2).upper() == "ENABLE")
        return None

    def create(
        self,
        *,
        name: str,
        query_sql: str,
        storage_table: str,
        refresh_mode: str = "interval",
        interval_sec: Optional[float] = None,
        source_tables: Optional[List[str]] = None,
        refresh_now: bool = True,
    ) -> Dict[str, Any]:
        if name in self._views:
            raise RuntimeError(f"materialized view already exists: {name}")
        spec = MaterializedViewSpec(
            name=name,
            query_sql=query_sql,
            storage_table=storage_table,
            refresh_mode=refresh_mode,
            interval_sec=float(interval_sec or self.default_interval_sec),
            source_tables=source_tables or self._infer_source_tables(query_sql),
        )
        self._ensure_storage(spec)
        self._views[name] = spec
        self._persist()
        if refresh_now:
            return self.refresh(name, reason="create")
        return spec.to_dict()

    def drop(self, name: str) -> Dict[str, Any]:
        spec = self._views.get(name)
        if spec is None:
            raise KeyError(f"materialized view not found: {name}")
        sql = f"DROP TABLE IF EXISTS {spec.storage_table};"
        for node in self.cluster.shard.nodes:
            self.cluster.client.query(sql, database=node.database)
        del self._views[name]
        self._persist()
        return {"dropped": name}

    def set_enabled(self, name: str, *, enabled: bool) -> Dict[str, Any]:
        spec = self._views.get(name)
        if spec is None:
            raise KeyError(f"materialized view not found: {name}")
        spec.enabled = enabled
        self._persist()
        return {"name": name, "enabled": enabled}

    def list(self) -> Dict[str, Any]:
        return {"count": len(self._views), "views": [v.to_dict() for v in self._views.values()]}

    def stats(self) -> Dict[str, Any]:
        out = dict(self._stats)
        refreshes = max(1.0, out.get("refreshes", 0.0))
        if out.get("refreshes", 0.0) > 0:
            out["latency_ms_mean"] = out["latency_ms_total"] / out["refreshes"]
            out["error_rate"] = out["errors"] / out["refreshes"]
        else:
            out["latency_ms_mean"] = 0.0
            out["error_rate"] = 0.0
        out["views"] = len(self._views)
        return out

    def _ensure_storage(self, spec: MaterializedViewSpec) -> None:
        sql = f"CREATE TABLE IF NOT EXISTS {spec.storage_table} (row_id INT, payload TEXT);"
        per_shard: Dict[str, Dict[str, Any]] = {}
        for node in self.cluster.shard.nodes:
            result = self.cluster.client.query(sql, database=node.database)
            per_shard[node.name] = {"database": str(node.database), "ok": bool(result.ok)}
        spec.per_shard = per_shard

    def refresh(self, name: str, *, reason: str = "manual") -> Dict[str, Any]:
        spec = self._views.get(name)
        if spec is None:
            raise KeyError(f"materialized view not found: {name}")
        if not spec.enabled:
            return {"name": name, "skipped": True, "reason": "disabled"}
        now = time.time()
        if spec.circuit_open_until > now:
            self._stats["skipped_circuit"] += 1.0
            raise RuntimeError(f"materialized view circuit open: {name}")
        started = time.perf_counter()
        try:
            per_shard, row_count = self._refresh_all_shards(spec)
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            spec.per_shard = per_shard
            spec.last_refresh_at = now
            spec.last_refresh_ms = elapsed_ms
            spec.rows_last_refresh = row_count
            spec.failures = 0
            spec.circuit_open_until = 0.0
            self._stats["refreshes"] += 1.0
            self._stats["latency_ms_total"] += elapsed_ms
            self._persist()
            return {
                "name": name,
                "reason": reason,
                "elapsed_ms": elapsed_ms,
                "rows": row_count,
                "per_shard": per_shard,
            }
        except Exception:
            spec.failures += 1
            self._stats["errors"] += 1.0
            if spec.failures >= self.max_failures:
                spec.circuit_open_until = time.time() + self.circuit_cooldown_sec
            self._persist()
            raise

    def _refresh_all_shards(self, spec: MaterializedViewSpec) -> Tuple[Dict[str, Dict[str, Any]], int]:
        per_shard: Dict[str, Dict[str, Any]] = {}
        total_rows = 0
        delete_sql = f"DELETE FROM {spec.storage_table};"
        for node in self.cluster.shard.nodes:
            del_result = self.cluster.client.query(delete_sql, database=node.database)
            sel_result = self.cluster.client.query(spec.query_sql, database=node.database)
            rows = parse_stdout_rows(sel_result.stdout)
            inserted = 0
            for i, row in enumerate(rows, start=1):
                payload = json.dumps(row, separators=(",", ":")).replace("'", "''")
                ins_sql = (
                    f"INSERT INTO {spec.storage_table} (row_id, payload) VALUES ({i}, '{payload}');"
                )
                ins_result = self.cluster.client.query(ins_sql, database=node.database)
                if ins_result.ok:
                    inserted += 1
            total_rows += inserted
            per_shard[node.name] = {
                "database": str(node.database),
                "delete_ok": bool(del_result.ok),
                "select_ok": bool(sel_result.ok),
                "inserted": inserted,
            }
        return per_shard, total_rows

    def on_mutation(self, sql: str) -> List[str]:
        if not self.enabled or not self.on_mutation_enabled:
            return []
        upper = sql.strip().upper()
        if upper.startswith("SELECT"):
            return []
        touched = self._tables_touched(sql)
        if not touched:
            return []
        now = time.time()
        refreshed: List[str] = []
        for name, spec in self._views.items():
            if spec.refresh_mode != "on_mutation" or not spec.enabled:
                continue
            if not (set(spec.source_tables) & touched):
                continue
            last = self._pending_mutation.get(name, 0.0)
            if now - last < self.mutation_debounce_sec:
                self._stats["skipped_debounce"] += 1.0
                continue
            self._pending_mutation[name] = now
            try:
                self.refresh(name, reason="mutation")
                refreshed.append(name)
            except RuntimeError:
                continue
        return refreshed

    @staticmethod
    def _tables_touched(sql: str) -> Set[str]:
        upper = sql.replace("\n", " ")
        parts = upper.split()
        touched: Set[str] = set()
        keywords = ("INTO", "UPDATE", "FROM", "TABLE")
        for i, token in enumerate(parts):
            if token in keywords and i + 1 < len(parts):
                table = parts[i + 1].strip(";,").lower()
                if table and table not in keywords:
                    touched.add(table)
        return touched

    def tick(self, *, force: bool = False) -> Dict[str, Any]:
        if not self.enabled:
            return {"enabled": False}
        now = time.time()
        due: List[str] = []
        refreshed: List[str] = []
        errors: List[Dict[str, str]] = []
        for name, spec in self._views.items():
            if spec.refresh_mode != "interval" or not spec.enabled:
                continue
            if force or (now - spec.last_refresh_at) >= spec.interval_sec:
                due.append(name)
        for name in due:
            try:
                self.refresh(name, reason="interval")
                refreshed.append(name)
            except Exception as exc:
                errors.append({"name": name, "error": str(exc)})
        return {
            "due": due,
            "refreshed": refreshed,
            "errors": errors,
            "stats": self.stats(),
        }
