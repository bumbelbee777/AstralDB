"""Detect drift between shards (fingerprints, hashes, custom probe SQL)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional

from quasar.client import AstralDBClient
from quasar.inventory import QuasarInventory, sha256_file
from quasar.quasar import QuasarShard


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
