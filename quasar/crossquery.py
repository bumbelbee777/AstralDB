"""Cross-shard query helpers: fan-out, scatter-gather, and parallel routed reads."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from quasar.merge import MergedFanoutResult, looks_like_read_query, merge_routed_results

if TYPE_CHECKING:
    from quasar.quasar import QuasarCluster, RoutedResult


def parse_stdout_rows(stdout: str) -> List[Dict[str, Any]]:
    """Best-effort row extraction from AstralDB CLI stdout (incl. mock ``OK:`` lines)."""
    rows: List[Dict[str, Any]] = []
    for line in stdout.splitlines():
        text = line.strip()
        if not text:
            continue
        if text.startswith("OK:"):
            payload = text[3:].strip()
            if payload:
                rows.append({"_line": payload})
        elif "|" in text and not text.startswith("-"):
            parts = [p.strip() for p in text.split("|")]
            if len(parts) >= 2:
                rows.append({f"col{i}": v for i, v in enumerate(parts)})
    return rows


@dataclass
class CrossQuerySpec:
    """Declarative cross-shard query."""

    sql: Optional[str] = None
    queries: List[Dict[str, str]] = field(default_factory=list)
    merge: bool = False
    parallel: bool = True
    include_rows: bool = False

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "CrossQuerySpec":
        return cls(
            sql=data.get("sql"),
            queries=list(data.get("queries", [])),
            merge=bool(data.get("merge", False)),
            parallel=bool(data.get("parallel", True)),
            include_rows=bool(data.get("include_rows", False)),
        )


@dataclass
class CrossQueryResult:
    merged: Optional[MergedFanoutResult] = None
    routed: List[Dict[str, Any]] = field(default_factory=list)
    rows: List[Dict[str, Any]] = field(default_factory=list)
    all_ok: bool = True

    def to_dict(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {"all_ok": self.all_ok, "row_count": len(self.rows)}
        if self.merged:
            out["merged"] = self.merged.to_dict()
        if self.routed:
            out["routed"] = self.routed
        if self.rows:
            out["rows"] = self.rows
        return out


class QuasarCrossQuery:
    """Higher-level cross-shard reads and parallel routed execution."""

    def __init__(self, cluster: "QuasarCluster") -> None:
        self.cluster = cluster

    def fanout(self, sql: str, *, merge: bool = False, parallel: bool = True) -> CrossQueryResult:
        if merge:
            merged = self.cluster.execute_merged(sql)
            rows = parse_stdout_rows(merged.merged_stdout) if looks_like_read_query(sql) else []
            return CrossQueryResult(merged=merged, rows=rows, all_ok=merged.all_ok)
        results = self._execute_fanout(sql, parallel=parallel)
        return self._pack_results(results, sql=sql, include_rows=True)

    def scatter_gather(self, queries: List[Dict[str, str]], *, parallel: bool = True) -> CrossQueryResult:
        """Run heterogeneous SQL on different shard keys in parallel."""
        if not queries:
            raise ValueError("queries list is required")

        def _run_one(item: Dict[str, str]) -> Dict[str, Any]:
            sql = item["sql"]
            shard_key = item.get("shard_key")
            routed = self.cluster.execute(sql, shard_key=shard_key)
            return {
                "sql": sql[:120],
                "shard_key": shard_key,
                "shards": [
                    {
                        "node": r.node,
                        "ok": r.result.ok,
                        "stdout": r.result.stdout,
                        "elapsed_ms": r.result.elapsed_ms,
                    }
                    for r in routed
                ],
            }

        out: List[Dict[str, Any]] = []
        all_ok = True
        if parallel and len(queries) > 1:
            with ThreadPoolExecutor(max_workers=len(queries)) as pool:
                futures = [pool.submit(_run_one, q) for q in queries]
                for fut in as_completed(futures):
                    item = fut.result()
                    out.append(item)
                    all_ok = all_ok and all(s["ok"] for s in item["shards"])
        else:
            for q in queries:
                item = _run_one(q)
                out.append(item)
                all_ok = all_ok and all(s["ok"] for s in item["shards"])
        rows: List[Dict[str, Any]] = []
        for item in out:
            for shard in item["shards"]:
                for row in parse_stdout_rows(shard.get("stdout", "")):
                    row = dict(row)
                    row["_shard"] = shard.get("node")
                    rows.append(row)
        return CrossQueryResult(routed=out, rows=rows, all_ok=all_ok)

    def execute(self, spec: CrossQuerySpec) -> CrossQueryResult:
        if spec.queries:
            return self.scatter_gather(spec.queries, parallel=spec.parallel)
        if not spec.sql:
            raise ValueError("sql or queries required")
        return self.fanout(spec.sql, merge=spec.merge, parallel=spec.parallel)

    def _execute_fanout(self, sql: str, *, parallel: bool) -> List["RoutedResult"]:
        shard = self.cluster.active_shard()
        if not parallel or len(shard.nodes) == 1:
            return shard.execute(sql, shard_key=None, parallel=False)
        return shard.execute(sql, shard_key=None, parallel=True)

    def _pack_results(
        self,
        results: List["RoutedResult"],
        *,
        sql: Optional[str] = None,
        include_rows: bool = False,
    ) -> CrossQueryResult:
        routed = [
            {
                "node": r.node,
                "database": str(r.database),
                "ok": r.result.ok,
                "stdout": r.result.stdout,
                "elapsed_ms": r.result.elapsed_ms,
            }
            for r in results
        ]
        all_ok = all(r.result.ok for r in results)
        rows: List[Dict[str, Any]] = []
        if include_rows:
            for r in results:
                for row in parse_stdout_rows(r.result.stdout):
                    tagged = dict(row)
                    tagged["_shard"] = r.node
                    rows.append(tagged)
        merged = None
        if sql and looks_like_read_query(sql):
            try:
                merged = merge_routed_results(results, sql=sql)
            except ValueError:
                pass
        return CrossQueryResult(merged=merged, routed=routed, rows=rows, all_ok=all_ok)

    def stream_fanout(self, sql: str, *, chunk_size: int = 250) -> List[Dict[str, Any]]:
        """Return read rows in bounded chunks for large cross-shard scans."""
        results = self._execute_fanout(sql, parallel=True)
        out: List[Dict[str, Any]] = []
        for r in results:
            rows = parse_stdout_rows(r.result.stdout)
            for i in range(0, len(rows), chunk_size):
                out.append({"shard": r.node, "rows": rows[i : i + chunk_size]})
        return out
