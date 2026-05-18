"""Distribution helpers: strict region sync and multi-shard coordination."""

from __future__ import annotations

import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from quasar.client import QueryResult


@dataclass
class RegionSyncResult:
    region: str
    ok: bool
    elapsed_ms: float
    error: Optional[str] = None


@dataclass
class StrictReplicationResult:
    primary: str
    replicas: List[RegionSyncResult] = field(default_factory=list)
    all_ok: bool = False

    def to_dict(self) -> Dict[str, Any]:
        return {
            "primary": self.primary,
            "all_ok": self.all_ok,
            "replicas": [
                {"region": r.region, "ok": r.ok, "elapsed_ms": r.elapsed_ms, "error": r.error}
                for r in self.replicas
            ],
        }


def strict_region_replicate(
    regions_module,
    sql: str,
    *,
    shard_key: str,
    timeout_sec: float = 30.0,
) -> StrictReplicationResult:
    """
    Replay write to every non-primary region and wait for all to complete.

    Use when ``regions_global.async_replicate`` is false or for settlement-critical writes.
    """
    write_region = regions_module._write_region
    primary_result = regions_module.write(sql, shard_key=shard_key)
    targets = [n for n in regions_module.regions if n != write_region]
    results: List[RegionSyncResult] = []

    def _repl(region_name: str) -> RegionSyncResult:
        start = time.perf_counter()
        try:
            shard = regions_module._shard_for_key(region_name, shard_key)
            regions_module.client.query(sql, database=shard.database, immediate=True)
            ms = (time.perf_counter() - start) * 1000.0
            return RegionSyncResult(region=region_name, ok=True, elapsed_ms=ms)
        except Exception as exc:
            ms = (time.perf_counter() - start) * 1000.0
            return RegionSyncResult(region=region_name, ok=False, elapsed_ms=ms, error=str(exc))

    with ThreadPoolExecutor(max_workers=max(1, len(targets))) as pool:
        futures = {pool.submit(_repl, r): r for r in targets}
        for fut in as_completed(futures, timeout=timeout_sec):
            results.append(fut.result())

    all_ok = all(r.ok for r in results)
    return StrictReplicationResult(primary=write_region, replicas=results, all_ok=all_ok)
