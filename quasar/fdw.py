"""FDW registry and connector adapter for Quasar."""

from __future__ import annotations

import json
import re
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Protocol, Tuple
from urllib.parse import quote_plus


@dataclass
class ForeignSource:
    name: str
    kind: str
    options: Dict[str, str]


@dataclass
class ForeignTable:
    name: str
    source: str
    object_name: str


@dataclass
class FdwQuery:
    columns: List[str]
    where_raw: Optional[str]
    limit: Optional[int]
    table_name: str
    predicates: List[Tuple[str, str, str]] | None = None


@dataclass
class FdwResult:
    rows: List[Dict[str, Any]]
    elapsed_ms: float
    source: str
    connector: str
    retries: int


class FdwConnector(Protocol):
    kind: str
    supports_projection_pushdown: bool
    supports_filter_pushdown: bool

    def query(self, source: ForeignSource, table: ForeignTable, query: FdwQuery) -> FdwResult:
        ...


class HttpJsonConnector:
    kind = "http_json"
    supports_projection_pushdown = True
    supports_filter_pushdown = True

    @staticmethod
    def _request(
        source: ForeignSource, object_name: str, query: FdwQuery, timeout: float
    ) -> List[Dict[str, Any]]:
        base_url = source.options.get("url")
        if not base_url:
            raise RuntimeError(f"foreign source '{source.name}' missing url option")
        url = base_url.rstrip("/") + "/" + object_name
        params: List[str] = []
        if query.columns and query.columns != ["*"]:
            params.append("fields=" + quote_plus(",".join(query.columns)))
        if query.predicates:
            filt = " AND ".join([f"{k}{op}{v}" for (k, op, v) in query.predicates])
            params.append("filter=" + quote_plus(filt))
        if query.limit is not None:
            params.append(f"limit={query.limit}")
        if params:
            url = url + ("&" if "?" in url else "?") + "&".join(params)
        headers = {"Accept": "application/json"}
        token = source.options.get("auth_token")
        if token:
            headers["Authorization"] = f"Bearer {token}"
        header_raw = source.options.get("auth_header")
        if header_raw and ":" in header_raw:
            hk, hv = header_raw.split(":", 1)
            headers[hk.strip()] = hv.strip()
        req = urllib.request.Request(url=url, method="GET", headers=headers)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
        if isinstance(payload, list):
            rows = payload
        elif isinstance(payload, dict) and isinstance(payload.get("rows"), list):
            rows = payload["rows"]
        else:
            raise RuntimeError(f"FDW response must be list or {{rows:[]}} for {url}")
        out = [r for r in rows if isinstance(r, dict)]
        if query.predicates:
            out = _apply_predicates(out, query.predicates)
        return out

    def query(self, source: ForeignSource, table: ForeignTable, query: FdwQuery) -> FdwResult:
        timeout = float(source.options.get("timeout_sec", 2.0))
        retry_max = max(0, int(source.options.get("retry_max", 2)))
        retry_backoff_ms = max(1, int(source.options.get("retry_backoff_ms", 50)))
        started = time.perf_counter()
        retries = 0
        while True:
            try:
                rows = self._request(source, table.object_name, query, timeout)
                elapsed = (time.perf_counter() - started) * 1000.0
                return FdwResult(
                    rows=rows,
                    elapsed_ms=elapsed,
                    source=source.name,
                    connector=self.kind,
                    retries=retries,
                )
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
                if retries >= retry_max:
                    raise RuntimeError("FDW fetch failed after retries") from exc
                retries += 1
                time.sleep((retry_backoff_ms * retries) / 1000.0)


class FdwManager:
    """Parses FDW DDL and executes foreign SELECT via pluggable connectors."""

    CREATE_SOURCE_RE = re.compile(
        r"^\s*CREATE\s+FOREIGN\s+SOURCE\s+([A-Za-z_][A-Za-z0-9_]*)\s+TYPE\s+([A-Za-z_][A-Za-z0-9_]*)\s+OPTIONS\s*\((.*)\)\s*;?\s*$",
        re.IGNORECASE | re.DOTALL,
    )
    CREATE_TABLE_RE = re.compile(
        r"^\s*CREATE\s+FOREIGN\s+TABLE\s+([A-Za-z_][A-Za-z0-9_]*)\s+SOURCE\s+([A-Za-z_][A-Za-z0-9_]*)\s+OBJECT\s+([A-Za-z_][A-Za-z0-9_]*)\s*;?\s*$",
        re.IGNORECASE,
    )
    SELECT_RE = re.compile(
        r"^\s*SELECT\s+(.*?)\s+FROM\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+WHERE\s+(.*?))?(?:\s+LIMIT\s+(\d+))?\s*;?\s*$",
        re.IGNORECASE | re.DOTALL,
    )
    _PRED_RE = re.compile(
        r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(=|!=|<=|>=|<|>)\s*('([^']*)'|[0-9]+(?:\.[0-9]+)?)\s*$",
        re.IGNORECASE,
    )

    def __init__(self, section: Optional[Dict[str, Any]] = None) -> None:
        section = section or {}
        self.sources: Dict[str, ForeignSource] = {}
        self.tables: Dict[str, ForeignTable] = {}
        self.enabled = bool(section.get("enabled", True))
        self.default_timeout_sec = float(section.get("timeout_sec", 2.0))
        self._circuit_open_until: Dict[str, float] = {}
        self._stats: Dict[str, float] = {
            "requests": 0.0,
            "errors": 0.0,
            "retries": 0.0,
            "latency_ms_total": 0.0,
            "circuit_rejections": 0.0,
        }
        self._source_stats: Dict[str, Dict[str, float]] = {}
        self._connectors: Dict[str, FdwConnector] = {"http_json": HttpJsonConnector()}

    @staticmethod
    def _parse_options(raw: str) -> Dict[str, str]:
        out: Dict[str, str] = {}
        for item in [x.strip() for x in raw.split(",") if x.strip()]:
            if "=" not in item:
                continue
            k, v = item.split("=", 1)
            out[k.strip().lower()] = v.strip().strip("'").strip('"')
        return out

    def try_apply_ddl(self, sql: str) -> Optional[Dict[str, Any]]:
        if not self.enabled:
            return None
        src = self.CREATE_SOURCE_RE.match(sql)
        if src:
            name, kind, opts = src.group(1), src.group(2).lower(), src.group(3)
            self.sources[name] = ForeignSource(name=name, kind=kind, options=self._parse_options(opts))
            return {"kind": "create_source", "name": name, "source_type": kind}
        table = self.CREATE_TABLE_RE.match(sql)
        if table:
            name, source, obj = table.group(1), table.group(2), table.group(3)
            if source not in self.sources:
                raise RuntimeError(f"foreign source not found: {source}")
            self.tables[name] = ForeignTable(name=name, source=source, object_name=obj)
            return {"kind": "create_foreign_table", "name": name, "source": source, "object": obj}
        return None

    def execute_select(self, sql: str) -> Optional[List[Dict[str, Any]]]:
        result = self.execute_select_with_meta(sql)
        if result is None:
            return None
        return result.rows

    def execute_select_with_meta(self, sql: str) -> Optional[FdwResult]:
        if not self.enabled:
            return None
        m = self.SELECT_RE.match(sql)
        if not m:
            return None
        cols_raw, table_name, where_raw, limit_raw = m.group(1), m.group(2), m.group(3), m.group(4)
        table = self.tables.get(table_name)
        if table is None:
            return None
        source = self.sources.get(table.source)
        if source is None:
            raise RuntimeError(f"foreign source missing for table: {table_name}")
        connector = self._connectors.get(source.kind)
        if connector is None:
            raise RuntimeError(f"unsupported foreign source type: {source.kind}")
        now = time.time()
        if self._circuit_open_until.get(source.name, 0.0) > now:
            self._stats["circuit_rejections"] += 1.0
            raise RuntimeError(f"FDW source circuit open: {source.name}")
        columns = [c.strip() for c in cols_raw.split(",")] if cols_raw.strip() else ["*"]
        limit = max(0, int(limit_raw)) if limit_raw else None
        query = FdwQuery(
            columns=columns,
            where_raw=where_raw,
            limit=limit,
            table_name=table_name,
            predicates=self._parse_predicates(where_raw),
        )
        self._stats["requests"] += 1.0
        per = self._source_stats.setdefault(source.name, {"requests": 0.0, "errors": 0.0, "latency_ms_total": 0.0})
        per["requests"] += 1.0
        try:
            res = connector.query(source, table, query)
            if query.limit is not None:
                res.rows = res.rows[: query.limit]
            self._stats["latency_ms_total"] += res.elapsed_ms
            self._stats["retries"] += float(res.retries)
            per["latency_ms_total"] += res.elapsed_ms
            return res
        except RuntimeError:
            self._stats["errors"] += 1.0
            per["errors"] += 1.0
            cooldown = float(source.options.get("circuit_cooldown_sec", 15.0))
            self._circuit_open_until[source.name] = time.time() + cooldown
            raise

    def _parse_predicates(self, where_raw: Optional[str]) -> List[Tuple[str, str, str]]:
        if not where_raw:
            return []
        terms = [t.strip() for t in re.split(r"\s+AND\s+", where_raw, flags=re.IGNORECASE) if t.strip()]
        out: List[Tuple[str, str, str]] = []
        for t in terms:
            m = self._PRED_RE.match(t)
            if not m:
                continue
            key, op, raw = m.group(1), m.group(2), m.group(3)
            out.append((key, op, raw.strip("'")))
        return out

    def stats(self) -> Dict[str, Any]:
        out: Dict[str, Any] = dict(self._stats)
        if out["requests"] > 0:
            out["latency_ms_mean"] = out["latency_ms_total"] / out["requests"]
            out["error_rate"] = out["errors"] / out["requests"]
        else:
            out["latency_ms_mean"] = 0.0
            out["error_rate"] = 0.0
        out["sources"] = {}
        for name, snap in self._source_stats.items():
            req = snap.get("requests", 0.0)
            out["sources"][name] = {
                **snap,
                "latency_ms_mean": snap["latency_ms_total"] / req if req else 0.0,
                "error_rate": snap["errors"] / req if req else 0.0,
            }
        return out


def _coerce_num(value: str) -> float | None:
    try:
        return float(value)
    except ValueError:
        return None


def _apply_predicates(
    rows: List[Dict[str, Any]],
    predicates: List[Tuple[str, str, str]],
) -> List[Dict[str, Any]]:
    def _ok(row: Dict[str, Any]) -> bool:
        for key, op, expected in predicates:
            actual = row.get(key)
            if actual is None:
                return False
            a_num = _coerce_num(str(actual))
            e_num = _coerce_num(expected)
            if a_num is not None and e_num is not None:
                left: Any = a_num
                right: Any = e_num
            else:
                left = str(actual)
                right = expected
            if op == "=" and not (left == right):
                return False
            if op == "!=" and not (left != right):
                return False
            if op == "<" and not (left < right):
                return False
            if op == ">" and not (left > right):
                return False
            if op == "<=" and not (left <= right):
                return False
            if op == ">=" and not (left >= right):
                return False
        return True

    return [r for r in rows if _ok(r)]
