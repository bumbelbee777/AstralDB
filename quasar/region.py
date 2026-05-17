"""Multi-region routing: primary writes, read replicas per geography, async replication."""

from __future__ import annotations

import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from quasar.client import AstralDBClient, QueryResult
from quasar.ring import ConsistentHashRing


@dataclass
class RegionShard:
    name: str
    database: Path


@dataclass
class RegionSpec:
    name: str
    shards: List[RegionShard]
    write_primary: bool = False
    priority: int = 0
    latency_bias_ms: float = 0.0


@dataclass
class RegionWriteResult:
    region: str
    shard: str
    result: QueryResult
    replicated_to: List[str] = field(default_factory=list)


class QuasarMultiRegion:
    """
    Geographic regions each hold a full copy of the shard map (different paths).

    * Writes go to the **write-primary** region, then async SQL replay to other regions.
    * Reads use the lowest ``priority`` region (0 = preferred/local).
    """

    def __init__(
        self,
        regions: Dict[str, RegionSpec],
        client: Optional[AstralDBClient] = None,
        *,
        virtual_nodes: int = 128,
        async_replicate: bool = True,
    ) -> None:
        if not regions:
            raise ValueError("at least one region required")
        self.regions = regions
        self.client = client or AstralDBClient()
        self.virtual_nodes = virtual_nodes
        self.async_replicate = async_replicate
        self._write_region = self._pick_write_region()
        self._read_order = sorted(regions.values(), key=lambda r: (r.priority, r.latency_bias_ms))
        shard_names = [s.name for s in next(iter(regions.values())).shards]
        self.ring = ConsistentHashRing(shard_names, virtual_nodes=virtual_nodes)
        self._shard_by_region: Dict[str, Dict[str, RegionShard]] = {
            rname: {s.name: s for s in spec.shards} for rname, spec in regions.items()
        }
        self._repl_executor = ThreadPoolExecutor(max_workers=8, thread_name_prefix="quasar-region-repl")

    def _pick_write_region(self) -> str:
        primaries = [n for n, s in self.regions.items() if s.write_primary]
        if primaries:
            return primaries[0]
        return min(self.regions.keys(), key=lambda n: self.regions[n].priority)

    def _shard_for_key(self, region: str, shard_key: str) -> RegionShard:
        shard_name = self.ring.node_for_key(shard_key)
        return self._shard_by_region[region][shard_name]

    def write(self, sql: str, *, shard_key: str) -> RegionWriteResult:
        primary = self._write_region
        target = self._shard_for_key(primary, shard_key)
        result = self.client.query(sql, database=target.database, immediate=True)
        replicated: List[str] = []
        if self.async_replicate:
            for rname, spec in self.regions.items():
                if rname == primary:
                    continue
                dest = self._shard_for_key(rname, shard_key)

                def _replay(region_name: str = rname, db: Path = dest.database, statement: str = sql) -> None:
                    try:
                        self.client.query(statement, database=db, immediate=True)
                    except RuntimeError:
                        pass

                self._repl_executor.submit(_replay)
                replicated.append(rname)
        return RegionWriteResult(
            region=primary,
            shard=target.name,
            result=result,
            replicated_to=replicated,
        )

    def read(self, sql: str, *, shard_key: str, region: Optional[str] = None) -> QueryResult:
        order = [self.regions[region]] if region else self._read_order
        last_error: Optional[Exception] = None
        for spec in order:
            target = self._shard_for_key(spec.name, shard_key)
            try:
                return self.client.query(sql, database=target.database, immediate=True)
            except RuntimeError as exc:
                last_error = exc
        raise RuntimeError("all regions failed for read") from last_error

    def health(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {}
        for rname, spec in self.regions.items():
            out[rname] = {
                s.name: self.client.health(database=s.database) for s in spec.shards
            }
        out["write_primary"] = self._write_region
        return out

    def close(self) -> None:
        self._repl_executor.shutdown(wait=False)

    @classmethod
    def from_config(cls, config: Dict[str, Any], client: Optional[AstralDBClient] = None) -> "QuasarMultiRegion":
        regions_cfg = config.get("regions", {})
        regions: Dict[str, RegionSpec] = {}
        for rname, spec in regions_cfg.items():
            shards = [
                RegionShard(name=s["name"], database=Path(s["database"]))
                for s in spec.get("shards", [])
            ]
            regions[rname] = RegionSpec(
                name=rname,
                shards=shards,
                write_primary=bool(spec.get("write_primary", False)),
                priority=int(spec.get("priority", 0)),
                latency_bias_ms=float(spec.get("latency_bias_ms", 0)),
            )
        global_cfg = config.get("regions_global", {})
        return cls(
            regions,
            client=client,
            virtual_nodes=int(config.get("virtual_nodes", 128)),
            async_replicate=bool(global_cfg.get("async_replicate", True)),
        )
