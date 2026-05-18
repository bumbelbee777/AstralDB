"""Detect drift between shards (fingerprints, hashes, custom probe SQL, replicas)."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from quasar.client import AstralDBClient
from quasar.inventory import QuasarInventory, sha256_file
from quasar.quasar import QuasarShard

if TYPE_CHECKING:
    from quasar.quasar import QuasarReplica


@dataclass
class DriftReport:
    method: str
    consistent: bool
    fingerprints: Dict[str, str]
    outliers: List[str]

    def to_dict(self) -> Dict[str, Any]:
        return {
            "method": self.method,
            "consistent": self.consistent,
            "fingerprints": self.fingerprints,
            "outliers": self.outliers,
        }


class QuasarDrift:
    """Compare shards by DB file hash and/or probe SQL output."""

    def __init__(self, shard: QuasarShard, client: Optional[AstralDBClient] = None) -> None:
        self.shard = shard
        self.client = client or shard.client

    def check_hashes(self) -> DriftReport:
        inv = QuasarInventory(self.shard.nodes)
        data = inv.collect(hash_files=True)
        fingerprints = {
            entry["name"]: entry.get("sha256") or "missing"
            for entry in data["shards"]
        }
        values = set(fingerprints.values())
        baseline = next(iter(fingerprints.values())) if fingerprints else None
        outliers = [n for n, fp in fingerprints.items() if fp != baseline]
        return DriftReport(
            method="sha256",
            consistent=len(values) <= 1,
            fingerprints=fingerprints,
            outliers=outliers if len(values) > 1 else [],
        )

    def check_probe(self, sql: str) -> DriftReport:
        """Run identical SQL on every shard; fingerprints are trimmed stdout."""
        fingerprints: Dict[str, str] = {}
        for node in self.shard.nodes:
            try:
                result = self.client.query(sql, database=node.database)
                fingerprints[node.name] = result.stdout.strip()
            except Exception as exc:  # noqa: BLE001
                fingerprints[node.name] = f"ERROR:{exc}"
        values = set(fingerprints.values())
        baseline = next(iter(values)) if values else None
        outliers = [n for n, v in fingerprints.items() if v != baseline]
        return DriftReport(
            method="probe_sql",
            consistent=len(values) <= 1,
            fingerprints=fingerprints,
            outliers=outliers if len(values) > 1 else [],
        )

    def check(
        self,
        *,
        probe_sql: Optional[str] = None,
        use_hashes: bool = True,
    ) -> List[DriftReport]:
        reports: List[DriftReport] = []
        if use_hashes:
            reports.append(self.check_hashes())
        if probe_sql:
            reports.append(self.check_probe(probe_sql))
        return reports

    @staticmethod
    def check_replica_set(rep: "QuasarReplica", *, label: str) -> DriftReport:
        """Compare master file hash to each replica (logical replication sanity)."""
        master = rep.set.master
        master_hash = sha256_file(master) if master.is_file() else "missing"
        fingerprints: Dict[str, str] = {"master": master_hash}
        outliers: List[str] = []
        for i, replica in enumerate(rep.set.replicas):
            key = f"replica_{i}"
            rhash = sha256_file(replica) if replica.is_file() else "missing"
            fingerprints[key] = rhash
            if rhash != master_hash:
                outliers.append(key)
        return DriftReport(
            method=f"replica_hash:{label}",
            consistent=len(outliers) == 0,
            fingerprints=fingerprints,
            outliers=outliers,
        )

    @staticmethod
    def check_all_replicas(replicas: Dict[str, "QuasarReplica"]) -> List[DriftReport]:
        return [QuasarDrift.check_replica_set(rep, label=name) for name, rep in replicas.items()]
