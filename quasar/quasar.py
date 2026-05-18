"""
Quasar — orchestration layer for AstralDB in production.

Wraps the AstralDB CLI executable for sharding, replication, backup, migration,
health checks, and an optional HTTP gateway.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
from contextlib import nullcontext
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Union

from quasar.client import AstralDBClient, QueryResult, find_astraldb
from quasar.config import (
    ConfigError,
    default_cluster_template,
    load_cluster_config,
    normalize_cluster_config,
    validate_cluster_config,
)
from quasar.errors import (
    QuasarCircuitOpenError,
    QuasarLockError,
    QuasarOverloadError,
    QuasarRestoreError,
    QuasarSecurityError,
)
from quasar.security import (
    SecurityPolicy,
    RateLimiter,
    constant_time_equal,
    resolve_path_under_base,
    validate_shard_key,
    validate_sql,
)
from quasar.ring import ConsistentHashRing

QUASAR_VERSION = "1.0.0"

PathLike = Union[str, Path]


@dataclass
class ShardNode:
    """One shard: logical name and on-disk AstralDB file."""

    name: str
    database: Path

    def wal_path(self) -> Path:
        return Path(str(self.database) + ".wal")


@dataclass
class RoutedResult:
    node: str
    database: Path
    result: QueryResult


class QuasarShard:
    """Route SQL to shard databases via consistent hashing."""

    def __init__(
        self,
        nodes: Sequence[ShardNode],
        client: Optional[AstralDBClient] = None,
        *,
        virtual_nodes: int = 128,
    ) -> None:
        if not nodes:
            raise ValueError("at least one shard node is required")
        self.nodes = list(nodes)
        self._by_name = {n.name: n for n in self.nodes}
        self.client = client or AstralDBClient()
        self.ring = ConsistentHashRing([n.name for n in self.nodes], virtual_nodes=virtual_nodes)

    def node_for_key(self, shard_key: str) -> ShardNode:
        name = self.ring.node_for_key(shard_key)
        return self._by_name[name]

    def _use_immediate(self) -> bool:
        return not hasattr(self.client, "pool_config")

    def execute(
        self,
        sql: str,
        *,
        shard_key: Optional[str] = None,
        parallel: bool = True,
    ) -> List[RoutedResult]:
        immediate = self._use_immediate()
        if shard_key is not None:
            node = self.node_for_key(shard_key)
            result = self.client.query(sql, database=node.database, immediate=immediate)
            return [RoutedResult(node=node.name, database=node.database, result=result)]

        targets = self.nodes
        if not parallel or len(targets) == 1:
            return [
                RoutedResult(
                    node=n.name,
                    database=n.database,
                    result=self.client.query(sql, database=n.database, immediate=immediate),
                )
                for n in targets
            ]

        out: List[RoutedResult] = []
        with ThreadPoolExecutor(max_workers=len(targets)) as pool:
            futures = {
                pool.submit(self.client.query, sql, database=n.database, immediate=immediate): n
                for n in targets
            }
            for fut in as_completed(futures):
                n = futures[fut]
                out.append(RoutedResult(node=n.name, database=n.database, result=fut.result()))
        return out

    def init_shard(self, sql: str, *, shard_key: str) -> RoutedResult:
        node = self.node_for_key(shard_key)
        result = self.client.query(sql, database=node.database)
        return RoutedResult(node=node.name, database=node.database, result=result)

    def health(self) -> Dict[str, bool]:
        return {n.name: self.client.health(database=n.database) for n in self.nodes}

    @classmethod
    def from_config(
        cls,
        config: Dict[str, Any],
        client: Optional[AstralDBClient] = None,
        *,
        config_path: Optional[PathLike] = None,
    ) -> "QuasarShard":
        normalized = normalize_cluster_config(config, config_path)
        nodes = [ShardNode(name=n["name"], database=Path(n["database"])) for n in normalized["shards"]]
        return cls(nodes, client=client, virtual_nodes=int(normalized.get("virtual_nodes", 128)))


@dataclass
class ReplicaSet:
    master: Path
    replicas: List[Path] = field(default_factory=list)

    @property
    def all_databases(self) -> List[Path]:
        return [self.master, *self.replicas]


class QuasarReplica:
    """Master + read replicas; writes propagate to every database file."""

    def __init__(self, replica_set: ReplicaSet, client: Optional[AstralDBClient] = None) -> None:
        self.set = replica_set
        self.client = client or AstralDBClient()

    def write(self, sql: str, *, sync_replicas: bool = True) -> List[QueryResult]:
        results: List[QueryResult] = []
        master_result = self.client.query(sql, database=self.set.master)
        results.append(master_result)
        if not sync_replicas:
            return results
        for replica in self.set.replicas:
            results.append(self.client.query(sql, database=replica))
        return results

    def write_parallel(self, sql: str) -> List[QueryResult]:
        dbs = self.set.all_databases
        results: List[QueryResult] = []
        with ThreadPoolExecutor(max_workers=len(dbs)) as pool:
            futures = [pool.submit(self.client.query, sql, database=db) for db in dbs]
            for fut in as_completed(futures):
                results.append(fut.result())
        return results

    def read(self, sql: str, *, prefer_replica: bool = True) -> QueryResult:
        if prefer_replica and self.set.replicas:
            import random

            db = random.choice(self.set.replicas)
        else:
            db = self.set.master
        return self.client.query(sql, database=db)

    def health(self) -> Dict[str, bool]:
        status: Dict[str, bool] = {}
        status["master"] = self.client.health(database=self.set.master)
        for i, replica in enumerate(self.set.replicas):
            status[f"replica_{i}"] = self.client.health(database=replica)
        return status

@dataclass
class BackupManifest:
    version: str
    created_at: str
    db_files: List[str]
    source_db: str
    incremental: bool = False
    parent_version: Optional[str] = None

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2)


class QuasarBackup:
    """Versioned full and incremental backups of AstralDB files (+ WAL, procedure cache)."""

    def __init__(self, backup_dir: PathLike) -> None:
        self.backup_dir = Path(backup_dir)
        self.backup_dir.mkdir(parents=True, exist_ok=True)

    def _wal_path(self, db_path: Path) -> Path:
        return Path(str(db_path) + ".wal")

    def _related_paths(self, db_path: Path) -> List[Path]:
        paths = [db_path]
        wal = self._wal_path(db_path)
        if wal.exists():
            paths.append(wal)
        parent = db_path.parent
        procs_json = parent / "astraldb_procs.json"
        procs_cache = parent / "astraldb_procs_cache"
        if procs_json.exists():
            paths.append(procs_json)
        if procs_cache.is_dir():
            paths.append(procs_cache)
        return paths

    def backup(self, db_path: PathLike, version: Optional[str] = None) -> Path:
        db_path = Path(db_path)
        version = version or datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
        dest = self.backup_dir / version
        dest.mkdir(parents=True, exist_ok=False)

        copied: List[str] = []
        for src in self._related_paths(db_path):
            if src.is_dir():
                shutil.copytree(src, dest / src.name)
                copied.append(src.name + "/")
            else:
                shutil.copy2(src, dest / src.name)
                copied.append(src.name)

        manifest = BackupManifest(
            version=version,
            created_at=datetime.now(timezone.utc).isoformat(),
            db_files=copied,
            source_db=str(db_path.resolve()),
            incremental=False,
        )
        (dest / "manifest.json").write_text(manifest.to_json(), encoding="utf-8")
        return dest

    def _assert_quiescent(self, target_db: Path, *, quiesce_sec: float, force: bool) -> None:
        """Warn if WAL was touched recently (likely an active writer)."""
        if force or quiesce_sec <= 0:
            return
        wal = self._wal_path(target_db)
        if not wal.exists():
            return
        age = time.time() - wal.stat().st_mtime
        if age < quiesce_sec:
            raise QuasarRestoreError(
                f"refusing restore: {wal.name} modified {age:.1f}s ago "
                f"(< {quiesce_sec}s quiesce window). Stop writers or pass force=True."
            )

    def restore(
        self,
        version: str,
        target_db: PathLike,
        *,
        overwrite: bool = False,
        force: bool = False,
        quiesce_sec: float = 2.0,
        checkpoint_source: bool = False,
        client: Optional["AstralDBClient"] = None,
    ) -> Path:
        src_dir = self.backup_dir / version
        if not src_dir.is_dir():
            raise FileNotFoundError(f"backup version not found: {version}")

        target_db = Path(target_db)
        self._assert_quiescent(target_db, quiesce_sec=quiesce_sec, force=force)
        if target_db.exists() and not overwrite:
            raise FileExistsError(f"refusing to overwrite existing database: {target_db}")
        if checkpoint_source and client is not None and target_db.exists():
            client.checkpoint_sql(database=target_db)

        target_db.parent.mkdir(parents=True, exist_ok=True)
        main_name = target_db.name
        if (src_dir / main_name).exists():
            shutil.copy2(src_dir / main_name, target_db)
        else:
            db_candidates = [p for p in src_dir.iterdir() if p.suffix == ".db" and p.is_file()]
            if not db_candidates:
                raise FileNotFoundError(f"no .db file in backup {version}")
            shutil.copy2(db_candidates[0], target_db)

        src_wal = src_dir / (target_db.name + ".wal")
        if not src_wal.exists():
            wal_candidates = sorted(src_dir.glob("*.wal"))
            src_wal = wal_candidates[0] if wal_candidates else src_wal
        if src_wal.exists():
            shutil.copy2(src_wal, self._wal_path(target_db))

        for extra in ("astraldb_procs.json", "astraldb_procs_cache"):
            src_extra = src_dir / extra
            if not src_extra.exists():
                continue
            dst = target_db.parent / extra
            if src_extra.is_dir():
                if dst.exists():
                    shutil.rmtree(dst)
                shutil.copytree(src_extra, dst)
            else:
                shutil.copy2(src_extra, dst)
        return target_db

    def incremental_backup(self, db_path: PathLike, version: Optional[str] = None) -> Optional[Path]:
        db_path = Path(db_path)
        wal = self._wal_path(db_path)
        if not wal.exists() or wal.stat().st_size == 0:
            return None
        version = version or datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S_wal")
        dest = self.backup_dir / version
        dest.mkdir(parents=True, exist_ok=False)
        shutil.copy2(wal, dest / wal.name)
        manifest = BackupManifest(
            version=version,
            created_at=datetime.now(timezone.utc).isoformat(),
            db_files=[wal.name],
            source_db=str(db_path.resolve()),
            incremental=True,
        )
        (dest / "manifest.json").write_text(manifest.to_json(), encoding="utf-8")
        return dest

    def list_versions(self) -> List[str]:
        if not self.backup_dir.is_dir():
            return []
        return sorted(
            p.name for p in self.backup_dir.iterdir() if p.is_dir() and (p / "manifest.json").exists()
        )

    def describe_version(self, version: str) -> BackupManifest:
        manifest_path = self.backup_dir / version / "manifest.json"
        if not manifest_path.is_file():
            raise FileNotFoundError(f"backup version not found: {version}")
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        return BackupManifest(**data)

    def prune(
        self,
        *,
        keep_last: Optional[int] = None,
        keep_days: Optional[float] = None,
        dry_run: bool = False,
    ) -> List[str]:
        """Delete old backup folders by count and/or age. Returns removed version names."""
        versions = self.list_versions()
        if not versions:
            return []

        to_remove: set[str] = set()
        if keep_last is not None and keep_last >= 0:
            excess = max(0, len(versions) - keep_last)
            to_remove.update(versions[:excess])

        if keep_days is not None and keep_days > 0:
            cutoff = datetime.now(timezone.utc).timestamp() - (keep_days * 86400.0)
            for version in versions:
                try:
                    manifest = self.describe_version(version)
                    created = datetime.fromisoformat(manifest.created_at.replace("Z", "+00:00"))
                    if created.timestamp() < cutoff:
                        to_remove.add(version)
                except (ValueError, FileNotFoundError, TypeError):
                    continue

        removed: List[str] = []
        for version in sorted(to_remove):
            target = self.backup_dir / version
            if dry_run:
                removed.append(version)
                continue
            if target.is_dir():
                shutil.rmtree(target)
                removed.append(version)
        return removed

class QuasarMigration:
    """Move data between AstralDB files using export/import bundle or file copy."""

    def __init__(
        self,
        source: PathLike,
        target: PathLike,
        client: Optional[AstralDBClient] = None,
        *,
        bundle_format: str = "json",
    ) -> None:
        self.source = Path(source)
        self.target = Path(target)
        self.client = client or AstralDBClient()
        self.bundle_format = bundle_format

    def migrate_via_bundle(self, work_dir: Optional[PathLike] = None) -> Path:
        work = Path(work_dir) if work_dir else Path.cwd() / ".quasar_migrate"
        work.mkdir(parents=True, exist_ok=True)
        bundle = work / f"migrate_{int(time.time())}.bundle.{self.bundle_format}"
        self.client.export_bundle(bundle, database=self.source, fmt=self.bundle_format)
        if self.target.exists():
            self.target.unlink()
        wal = Path(str(self.target) + ".wal")
        if wal.exists():
            wal.unlink()
        self.client.import_bundle(bundle, database=self.target, fmt=self.bundle_format)
        return bundle

    def migrate_via_checkpoint_copy(self) -> None:
        """Checkpoint source, copy main + WAL (+ procedure cache) to target path."""
        self.client.checkpoint_sql(database=self.source)
        self.target.parent.mkdir(parents=True, exist_ok=True)
        if self.target.exists():
            self.target.unlink()
        shutil.copy2(self.source, self.target)
        src_wal = Path(str(self.source) + ".wal")
        if src_wal.exists():
            shutil.copy2(src_wal, Path(str(self.target) + ".wal"))
        parent = self.source.parent
        for extra in ("astraldb_procs.json", "astraldb_procs_cache"):
            src = parent / extra
            if not src.exists():
                continue
            dst = self.target.parent / extra
            if src.is_dir():
                if dst.exists():
                    shutil.rmtree(dst)
                shutil.copytree(src, dst)
            else:
                shutil.copy2(src, dst)


class QuasarMonitor:
    """Lightweight in-process metrics for Quasar-driven workloads."""

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.query_count = 0
        self.error_count = 0
        self.total_latency_ms = 0.0
        self.latencies_ms: List[float] = []

    def record(self, result: QueryResult, *, error: bool = False) -> None:
        self.query_count += 1
        self.total_latency_ms += result.elapsed_ms
        self.latencies_ms.append(result.elapsed_ms)
        if error or not result.ok:
            self.error_count += 1

    @property
    def error_rate(self) -> float:
        if self.query_count == 0:
            return 0.0
        return self.error_count / self.query_count

    @property
    def mean_latency_ms(self) -> float:
        if self.query_count == 0:
            return 0.0
        return self.total_latency_ms / self.query_count

    def percentile(self, p: float) -> float:
        if not self.latencies_ms:
            return 0.0
        ordered = sorted(self.latencies_ms)
        idx = min(len(ordered) - 1, int(round((p / 100.0) * (len(ordered) - 1))))
        return ordered[idx]

    def snapshot(self) -> Dict[str, float]:
        return {
            "queries": float(self.query_count),
            "errors": float(self.error_count),
            "error_rate": self.error_rate,
            "mean_latency_ms": self.mean_latency_ms,
            "p50_latency_ms": self.percentile(50),
            "p99_latency_ms": self.percentile(99),
        }

class QuasarCluster:
    """Named cluster: sharded writers + optional replica sets from JSON config."""

    def __init__(
        self,
        config: Dict[str, Any],
        client: Optional[AstralDBClient] = None,
        *,
        config_path: Optional[PathLike] = None,
    ) -> None:
        self.config_path = Path(config_path) if config_path else None
        self.security = SecurityPolicy.from_config(config.get("security"))
        self.config = normalize_cluster_config(config, config_path, self.security)
        self.client = client or self._create_client()
        self.shard = QuasarShard.from_config(self.config, client=self.client, config_path=config_path)
        pool_cfg = self.config.get("pool", {})
        if pool_cfg.get("enabled", True) and pool_cfg.get("warm_on_start", True):
            if hasattr(self.client, "warm"):
                self.client.warm([n.database for n in self.shard.nodes])
        from quasar.workload import ShardCircuitBreaker, WorkloadGuard

        from quasar.recovery import RetryPolicy

        wl = self.config.get("workload", {})
        breaker = ShardCircuitBreaker()
        breaker.config.failure_threshold = int(wl.get("circuit_failure_threshold", 5))
        breaker.config.cooldown_sec = float(wl.get("circuit_cooldown_sec", 30))
        breaker.config.half_open_max_probes = int(wl.get("circuit_half_open_max_probes", 1))
        self._retry_policy = RetryPolicy.from_config(self.config.get("recovery"))
        self.workload = WorkloadGuard(breaker, retry_policy=self._retry_policy)
        self.regions = self._build_regions()
        self.replicas: Dict[str, QuasarReplica] = {}
        for name, spec in self.config.get("replicas", {}).items():
            self.replicas[name] = QuasarReplica(
                ReplicaSet(master=Path(spec["master"]), replicas=[Path(p) for p in spec.get("slaves", [])]),
                client=self.client,
            )
        self.monitor = QuasarMonitor()
        backup_dir = self.config.get("backup_dir")
        self.backup = QuasarBackup(backup_dir) if backup_dir else None
        self.failover = self._build_failover()
        self.consensus = self._build_consensus()
        self.dtxn = self._build_dtxn()
        self.recovery_auto = self._build_recovery_auto()
        self.autoscale = self._build_autoscale()
        self.cross_query_api = None
        if self.config.get("cross_query", {}).get("enabled", True):
            from quasar.crossquery import QuasarCrossQuery

            self.cross_query_api = QuasarCrossQuery(self)
        self.multimaster = self._build_multimaster()
        rb_cfg = self.config.get("rebalance", {})
        from quasar.rebalance import QuasarRebalance

        self.rebalance = QuasarRebalance(
            self.shard,
            client=self.client,
            shard_key_column=rb_cfg.get("shard_key_column", "id"),
            virtual_nodes=int(self.config.get("virtual_nodes", 128)),
        )
        self.rebalance_auto = self._build_rebalance_auto()
        locks_cfg = self.config.get("locks", {})
        self._locks_enabled = bool(locks_cfg.get("enabled", False))
        self._lock_timeout_sec = float(locks_cfg.get("timeout_sec", 30.0))
        self._build_financial()

    def _build_financial(self) -> None:
        from quasar.audit import QuasarJournal
        from quasar.idempotency import IdempotencyConfig, IdempotencyStore
        from quasar.ledger import FinancialConfig, QuasarLedger, QuasarReconciler
        from quasar.saga import QuasarSaga

        fin_raw = self.config.get("financial", {}) or {}
        self.financial = FinancialConfig.from_config(fin_raw)
        self.journal: Optional[QuasarJournal] = None
        self.idempotency: Optional[IdempotencyStore] = None
        self.saga: Optional[QuasarSaga] = None
        self.ledger: Optional[QuasarLedger] = None
        self.reconciler: Optional[QuasarReconciler] = None
        if not self.financial.enabled:
            return
        journal_path = Path(fin_raw.get("journal_file", ".quasar/journal.jsonl"))
        self.journal = QuasarJournal(journal_path)
        idem_dir = Path(fin_raw.get("idempotency_dir", ".quasar/idempotency"))
        idem_cfg = IdempotencyConfig.from_config(fin_raw.get("idempotency"))
        self.idempotency = IdempotencyStore(self, idem_cfg, base_dir=idem_dir)
        saga_dir = Path(fin_raw.get("saga_state_dir", ".quasar/sagas"))
        self.saga = QuasarSaga(self, state_dir=saga_dir, journal=self.journal)
        self.ledger = QuasarLedger(self, self.financial, saga=self.saga)
        self.reconciler = QuasarReconciler(self)

    def _create_client(self) -> AstralDBClient:
        from quasar.pool import create_client

        pool_cfg = self.config.get("pool", {})
        use_pool = bool(pool_cfg.get("enabled", True))
        pool_args = {k: v for k, v in pool_cfg.items() if k != "enabled"}
        return create_client(
            pool_config=pool_args,
            use_pool=use_pool,
            security_policy=self.security,
            recovery_config=self.config.get("recovery"),
            mvcc_config=self.config.get("mvcc"),
        )

    def close(self) -> None:
        """Release pool workers, region replicator, etc."""
        if self.recovery_auto is not None:
            self.recovery_auto.stop_background()
        if hasattr(self.client, "close"):
            self.client.close()
        if self.regions is not None:
            self.regions.close()

    def recovery_tick(self, *, force: bool = False) -> Dict[str, Any]:
        """Automated crash recovery (dtxn + rollbacks + pool heal)."""
        if self.recovery_auto is None:
            if force:
                from quasar.recovery_auto import CrashRecoveryManager, RecoveryAutomationConfig

                mgr = CrashRecoveryManager(self, RecoveryAutomationConfig(enabled=True))
                return mgr.run(force=True)
            return {"skipped": True}
        return self.recovery_auto.run(force=force)

    def autoscale_tick(self, *, apply: Optional[bool] = None) -> Dict[str, Any]:
        """Evaluate metrics and optionally add shards (see ``autoscaling`` config)."""
        if self.autoscale is None:
            from quasar.autoscale import AutoscaleConfig, QuasarAutoscale

            mgr = QuasarAutoscale(self, AutoscaleConfig(enabled=True))
            report = mgr.tick(apply=apply)
        else:
            report = self.autoscale.tick(apply=apply)
        if report.get("applied") and self.rebalance_auto is not None:
            rb_raw = self.config.get("rebalance", {})
            if rb_raw.get("auto_apply") or self.config.get("autoscaling", {}).get("rebalance_on_scale"):
                report["rebalance"] = self.rebalance_auto.tick(force_apply=True)
        return report

    def rebalance_tick(self, *, force_apply: Optional[bool] = None) -> Dict[str, Any]:
        """Automatic rebalance plan/apply (see ``rebalance`` automation keys)."""
        if self.rebalance_auto is None:
            from quasar.rebalance_auto import QuasarRebalanceAutomator, RebalanceAutomationConfig

            auto = QuasarRebalanceAutomator(self, RebalanceAutomationConfig(enabled=True))
            return auto.tick(force_apply=force_apply)
        return self.rebalance_auto.tick(force_apply=force_apply)

    def consensus_status(self) -> Dict[str, Any]:
        if self.consensus is None:
            return {"enabled": False}
        return self.consensus.status()

    def cross_query(
        self,
        sql: Optional[str] = None,
        *,
        merge: bool = False,
        queries: Optional[List[Dict[str, str]]] = None,
        parallel: bool = True,
    ):
        if self.cross_query_api is None:
            from quasar.crossquery import QuasarCrossQuery

            self.cross_query_api = QuasarCrossQuery(self)
        if queries:
            return self.cross_query_api.scatter_gather(queries, parallel=parallel)
        if not sql:
            raise ValueError("sql or queries required")
        return self.cross_query_api.fanout(sql, merge=merge, parallel=parallel)

    def _build_regions(self):
        if not self.config.get("regions"):
            return None
        from quasar.region import QuasarMultiRegion

        return QuasarMultiRegion.from_config(self.config, client=self.client)

    def _build_failover(self):
        fo = self.config.get("failover", {})
        if not fo.get("enabled"):
            return None
        from quasar.failover import FailoverPolicy, FailoverTarget, QuasarFailover

        targets: Dict[str, FailoverTarget] = {}
        for name, spec in fo.get("shards", {}).items():
            targets[name] = FailoverTarget(
                shard_name=name,
                primary=Path(spec["primary"]),
                standbys=[Path(p) for p in spec.get("standbys", [])],
            )
        state_path = Path(fo.get("state_file", ".quasar/failover.json"))
        policy = FailoverPolicy.from_config(fo)
        return QuasarFailover(
            self.shard,
            targets,
            client=self.client,
            state_file=state_path,
            auto_promote=bool(fo.get("auto_promote", True)),
            policy=policy,
        )

    def reload_shards(self) -> None:
        """Reload shard ring and dependent planners after ``cluster.json`` changes."""
        self.shard = QuasarShard.from_config(
            self.config, client=self.client, config_path=self.config_path
        )
        rb_cfg = self.config.get("rebalance", {})
        from quasar.rebalance import QuasarRebalance

        self.rebalance = QuasarRebalance(
            self.shard,
            client=self.client,
            shard_key_column=rb_cfg.get("shard_key_column", "id"),
            virtual_nodes=int(self.config.get("virtual_nodes", 128)),
        )
        if self.dtxn is not None:
            self.dtxn.shard = self.shard
            self.dtxn._by_name = {n.name: n for n in self.shard.nodes}

    def _build_consensus(self):
        from quasar.consensus import build_consensus

        cons = build_consensus(self)
        if cons is not None and not cons.is_leader():
            cons.elect_leader()
        return cons

    def _build_rebalance_auto(self):
        from quasar.rebalance_auto import QuasarRebalanceAutomator, RebalanceAutomationConfig

        rb = self.config.get("rebalance", {})
        auto_raw = rb.get("automation")
        if isinstance(auto_raw, dict):
            cfg = RebalanceAutomationConfig.from_config(auto_raw)
        else:
            cfg = RebalanceAutomationConfig(
                enabled=bool(rb.get("auto_apply")) or float(rb.get("interval_sec", 0)) > 0,
                auto_apply=bool(rb.get("auto_apply")),
                interval_sec=float(rb.get("interval_sec", 0)),
                delete_from_source=bool(rb.get("delete_from_source", False)),
            )
        if not cfg.enabled:
            return None
        return QuasarRebalanceAutomator(self, cfg)

    def _build_autoscale(self):
        from quasar.autoscale import AutoscaleConfig, QuasarAutoscale

        raw = self.config.get("autoscaling", {})
        cfg = AutoscaleConfig.from_config(raw)
        if not cfg.enabled:
            return None
        return QuasarAutoscale(self, cfg)

    def _build_recovery_auto(self):
        from quasar.recovery_auto import CrashRecoveryManager, RecoveryAutomationConfig

        raw = self.config.get("recovery_automation", {})
        cfg = RecoveryAutomationConfig.from_config(raw)
        if not cfg.enabled:
            return None
        mgr = CrashRecoveryManager(self, cfg)
        if cfg.on_start:
            mgr.run()
        mgr.start_background()
        return mgr

    def _build_dtxn(self):
        from quasar.dtxn import DistributedTransactionCoordinator, DistributedTxnConfig

        raw = self.config.get("distributed_txn", {})
        base = self.config_path.parent if self.config_path else None
        cfg = DistributedTxnConfig.from_config(raw, base)
        if not cfg.enabled:
            return None
        coord = DistributedTransactionCoordinator(
            self.shard,
            self.client,
            wal_path=Path(cfg.wal_file),
            participant_log_dir=Path(cfg.participant_log_dir),
            config=cfg,
            consensus=self.consensus,
        )
        if cfg.recover_on_start:
            coord.recover()
        return coord

    def _build_multimaster(self):
        mm = self.config.get("multi_master")
        if not mm:
            return None
        from quasar.multimaster import MultiMasterGroup, QuasarMultiMaster

        groups = {
            name: MultiMasterGroup(
                shard_name=name,
                writers=[Path(p) for p in spec.get("writers", [])],
                quorum=int(spec.get("quorum", 1)),
            )
            for name, spec in mm.items()
        }
        return QuasarMultiMaster(self.shard, groups, client=self.client)

    def active_shard(self) -> QuasarShard:
        """Shard view with failover-resolved database paths."""
        if self.failover is None:
            return self.shard
        from quasar.failover import QuasarFailover

        nodes = self.failover.resolve_nodes()
        return QuasarShard(
            nodes,
            client=self.client,
            virtual_nodes=int(self.config.get("virtual_nodes", 128)),
        )

    @classmethod
    def from_file(cls, path: PathLike, client: Optional[AstralDBClient] = None) -> "QuasarCluster":
        config_path = Path(path)
        return cls(load_cluster_config(config_path), client=client, config_path=config_path)

    def _lock_targets(self, shard_key: Optional[str]) -> List[Path]:
        active = self.active_shard()
        if shard_key is not None:
            return [active.node_for_key(shard_key).database]
        return [n.database for n in active.nodes]

    def execute(
        self,
        sql: str,
        *,
        shard_key: Optional[str] = None,
        multi_master: bool = False,
    ) -> List[RoutedResult]:
        validate_sql(sql, self.security)
        if shard_key is not None:
            validate_shard_key(shard_key, self.security)
        return self._execute_unlocked(sql, shard_key=shard_key, multi_master=multi_master)

    def execute_merged(
        self,
        sql: str,
        *,
        shard_key: Optional[str] = None,
    ):
        from quasar.merge import merge_routed_results

        results = self._execute_unlocked(sql, shard_key=shard_key, multi_master=False)
        merged = merge_routed_results(results, sql=sql)
        if not merged.all_ok:
            raise RuntimeError("fan-out merge: one or more shards failed")
        return merged

    def _execute_unlocked(
        self,
        sql: str,
        *,
        shard_key: Optional[str] = None,
        multi_master: bool = False,
    ) -> List[RoutedResult]:
        if multi_master and shard_key and self.multimaster:
            mm = self.multimaster.write(sql, shard_key=shard_key)
            writers = self.multimaster.groups[mm.shard].writers
            routed = [
                RoutedResult(node=mm.shard, database=writers[i], result=r)
                for i, r in enumerate(mm.results)
            ]
            for r in routed:
                self.monitor.record(r.result)
            return routed

        lock_ctx = nullcontext()
        if self._locks_enabled:
            from quasar.locks import lock_databases

            lock_ctx = lock_databases(
                self._lock_targets(shard_key),
                timeout_sec=self._lock_timeout_sec,
            )
        try:
            with lock_ctx:
                active = self.active_shard()

                def _run() -> List[RoutedResult]:
                    return active.execute(sql, shard_key=shard_key)

                routed_via_workload = False
                if shard_key:
                    node = active.node_for_key(shard_key)
                    results = self.workload.run(node.name, _run)
                    routed_via_workload = True
                elif self._retry_policy.max_retries > 0:
                    from quasar.recovery import execute_with_retry

                    results = execute_with_retry(
                        _run,
                        self._retry_policy,
                        stats=self.workload.retry_stats,
                    )
                else:
                    results = _run()

                for r in results:
                    self.monitor.record(r.result)
                    if not routed_via_workload:
                        if r.result.ok:
                            self.workload.success(r.node)
                        else:
                            self.workload.failure(r.node)
                return results
        except QuasarLockError:
            raise QuasarOverloadError("could not acquire database lock") from None

    def cross_join(self, spec_path: PathLike) -> List[Dict[str, Any]]:
        from quasar.crossjoin import CrossJoinSpec, QuasarCrossShardJoin

        path = Path(spec_path)
        if self.config_path:
            base = self.config_path.parent
            path = resolve_path_under_base(
                base,
                str(path) if path.is_absolute() else str(spec_path),
                allow_outside=self.security.allow_path_outside_config_root,
            )
        spec = CrossJoinSpec.from_file(path, policy=self.security)
        for table in spec.tables:
            validate_sql(table.sql, self.security)
        return QuasarCrossShardJoin(self.active_shard(), client=self.client).execute(spec)

    def rebalance_plan(self, new_shard_names: List[str], **kwargs: Any) -> Dict[str, Any]:
        return self.rebalance.plan(new_shard_names, **kwargs).to_dict()

    def rebalance_apply(self, plan_dict: Dict[str, Any], **kwargs: Any) -> Dict[str, int]:
        from quasar.rebalance import RebalanceMove, RebalancePlan

        plan = RebalancePlan(
            shard_key_column=plan_dict["shard_key_column"],
            moves=[RebalanceMove(**m) for m in plan_dict.get("moves", [])],
            summary=plan_dict.get("summary", {}),
        )
        return self.rebalance.apply(plan, **kwargs)

    def failover_tick(self) -> List[Dict[str, Any]]:
        if self.failover is None:
            return []
        return self.failover.tick()

    def cross_shard_transaction(self, statements: Optional[List[Dict[str, str]]] = None):
        from quasar.xtxn import CrossShardTransaction, TxnStatement

        if statements:
            stmts = [TxnStatement(sql=s["sql"], shard_key=s["shard_key"]) for s in statements]
            if self.dtxn is not None:
                return self.dtxn.run(stmts)
            txn = CrossShardTransaction(self.active_shard(), client=self.client)
            return txn.run(stmts)
        if self.dtxn is not None:
            return self.dtxn
        return CrossShardTransaction(self.active_shard(), client=self.client)

    def dtxn_recover(self) -> List[Dict[str, Any]]:
        if self.dtxn is None:
            raise RuntimeError("distributed_txn not enabled in cluster config")
        return self.dtxn.recover()

    def pool_stats(self) -> Dict[str, float]:
        if hasattr(self.client, "pool_stats"):
            return self.client.pool_stats()
        return {}

    def workload_stats(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {
            "stats": self.workload.stats.to_dict(),
            "circuits": self.workload.breaker.snapshot(),
            "retry": self.workload.retry_stats.to_dict(),
        }
        if hasattr(self.client, "retry_stats"):
            out["pool_retry"] = self.client.retry_stats.to_dict()
        return out

    def region_write(self, sql: str, *, shard_key: str):
        if self.regions is None:
            raise RuntimeError("regions not configured")
        return self.regions.write(sql, shard_key=shard_key)

    def region_read(self, sql: str, *, shard_key: str, region: Optional[str] = None):
        if self.regions is None:
            raise RuntimeError("regions not configured")
        return self.regions.read(sql, shard_key=shard_key, region=region)

    def run_script(
        self,
        script: PathLike,
        *,
        shard_key: Optional[str] = None,
        parallel: bool = True,
    ) -> List[RoutedResult]:
        script = Path(script)
        if shard_key is not None:
            node = self.shard.node_for_key(shard_key)
            result = self.client.script(script, database=node.database)
            routed = RoutedResult(node=node.name, database=node.database, result=result)
            self.monitor.record(result)
            return [routed]

        out: List[RoutedResult] = []
        nodes = self.shard.nodes
        if not parallel or len(nodes) == 1:
            for n in nodes:
                result = self.client.script(script, database=n.database)
                self.monitor.record(result)
                out.append(RoutedResult(node=n.name, database=n.database, result=result))
            return out

        with ThreadPoolExecutor(max_workers=len(nodes)) as pool:
            futures = {pool.submit(self.client.script, script, database=n.database): n for n in nodes}
            for fut in as_completed(futures):
                n = futures[fut]
                result = fut.result()
                self.monitor.record(result)
                out.append(RoutedResult(node=n.name, database=n.database, result=result))
        return out

    def backup_all(self, *, incremental: bool = False, label_prefix: str = "") -> Dict[str, Optional[Path]]:
        if self.backup is None:
            raise RuntimeError("cluster config has no backup_dir")
        stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
        results: Dict[str, Optional[Path]] = {}
        for node in self.shard.nodes:
            version = f"{label_prefix}{node.name}_{stamp}" if label_prefix else f"{node.name}_{stamp}"
            if incremental:
                results[node.name] = self.backup.incremental_backup(node.database, version=version)
            else:
                results[node.name] = self.backup.backup(node.database, version=version)
        retention = self.config.get("backup_retention")
        if retention and self.backup:
            self.backup.prune(
                keep_last=retention.get("keep_last"),
                keep_days=retention.get("keep_days"),
            )
        return results

    def prune_backups(self, *, dry_run: bool = False) -> List[str]:
        if self.backup is None:
            raise RuntimeError("cluster config has no backup_dir")
        retention = self.config.get("backup_retention", {})
        return self.backup.prune(
            keep_last=retention.get("keep_last"),
            keep_days=retention.get("keep_days"),
            dry_run=dry_run,
        )

    def health(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {
            "quasar_version": QUASAR_VERSION,
            "astraldb": self.client.version(),
            "shards": self.shard.health(),
        }
        if self.replicas:
            out["replicas"] = {k: v.health() for k, v in self.replicas.items()}
        out["healthy"] = all(out["shards"].values()) and all(
            all(v.values()) for v in out.get("replicas", {}).values()
        )
        return out

    def status(self, *, hash_files: bool = False) -> Dict[str, Any]:
        from quasar.inventory import QuasarInventory

        inv = QuasarInventory(self.shard.nodes).collect(hash_files=hash_files)
        return {
            "health": self.health(),
            "inventory": inv,
            "config_path": str(self.config_path) if self.config_path else None,
        }

    def execute_idempotent(
        self,
        sql: str,
        *,
        shard_key: str,
        idempotency_key: str,
    ) -> List[RoutedResult]:
        """Run SQL once per idempotency key (file or table store)."""
        if self.idempotency is None:
            raise RuntimeError("financial.idempotency not enabled in cluster config")
        from quasar.idempotency import digest_result

        existing = self.idempotency.get(idempotency_key, shard_key=shard_key)
        if existing and existing.get("status") == "completed":
            if self.journal:
                self.journal.record(
                    "IDEMPOTENT_REPLAY",
                    detail=idempotency_key,
                    idempotency_key=idempotency_key,
                )
            node = self.shard.node_for_key(shard_key)
            from quasar.client import QueryResult

            cached = QueryResult(
                stdout=f"IDEMPOTENT:{existing.get('response_digest', '')}",
                stderr="",
                returncode=0,
                elapsed_ms=0.0,
            )
            return [RoutedResult(node=node.name, database=node.database, result=cached)]
        self.idempotency.begin(idempotency_key, shard_key=shard_key)
        try:
            results = self.execute(sql, shard_key=shard_key)
            digest = digest_result([r.result.stdout for r in results])
            self.idempotency.complete(idempotency_key, shard_key=shard_key, digest=digest)
            if self.journal:
                self.journal.record(
                    "IDEMPOTENT_OK",
                    detail=idempotency_key,
                    idempotency_key=idempotency_key,
                )
            return results
        except Exception:
            self.idempotency.fail(idempotency_key, shard_key=shard_key)
            if self.journal:
                self.journal.record(
                    "IDEMPOTENT_FAIL",
                    detail=idempotency_key,
                    outcome="FAIL",
                    idempotency_key=idempotency_key,
                )
            raise

    def run_saga(self, spec: Dict[str, Any]):
        if self.saga is None:
            raise RuntimeError("financial not enabled in cluster config")
        return self.saga.run_spec(spec)

    def transfer(self, request: Dict[str, Any]):
        if self.ledger is None:
            raise RuntimeError("financial not enabled in cluster config")
        from quasar.ledger import TransferRequest

        req = TransferRequest(
            txn_id=str(request["txn_id"]),
            from_account=str(request["from_account"]),
            to_account=str(request["to_account"]),
            amount_cents=int(request["amount_cents"]),
            from_shard_key=str(request["from_shard_key"]),
            to_shard_key=str(request["to_shard_key"]),
            currency=str(request.get("currency", self.financial.currency)),
        )
        if self.journal:
            self.journal.record("TRANSFER_START", detail=req.txn_id, extra=request)
        result = self.ledger.transfer(req)
        if self.journal:
            self.journal.record("TRANSFER_END", detail=req.txn_id, outcome=result.status)
        return result

    def reconcile(self, *, probe_sql: Optional[str] = None):
        if self.reconciler is None:
            raise RuntimeError("financial not enabled in cluster config")
        if probe_sql:
            return self.reconciler.probe(probe_sql)
        return self.reconciler.sum_balances()

    def region_write_strict(self, sql: str, *, shard_key: str):
        if self.regions is None:
            raise RuntimeError("regions not configured")
        from quasar.distribute import strict_region_replicate

        return strict_region_replicate(self.regions, sql, shard_key=shard_key)

    def check_drift(
        self,
        *,
        probe_sql: Optional[str] = None,
        use_hashes: bool = True,
        check_replicas: bool = False,
    ) -> List[Dict[str, Any]]:
        from quasar.drift import QuasarDrift

        drift = QuasarDrift(self.shard, client=self.client)
        reports = [r.to_dict() for r in drift.check(probe_sql=probe_sql, use_hashes=use_hashes)]
        if check_replicas and self.replicas:
            reports.extend(r.to_dict() for r in QuasarDrift.check_all_replicas(self.replicas))
        return reports


def format_prometheus_metrics(cluster: QuasarCluster) -> str:
    snap = cluster.monitor.snapshot()
    health = cluster.health()
    lines = [
        "# HELP quasar_queries_total Queries executed through Quasar",
        "# TYPE quasar_queries_total counter",
        f"quasar_queries_total {int(snap['queries'])}",
        "# HELP quasar_errors_total Failed queries",
        "# TYPE quasar_errors_total counter",
        f"quasar_errors_total {int(snap['errors'])}",
        "# HELP quasar_latency_ms Mean query latency",
        "# TYPE quasar_latency_ms gauge",
        f"quasar_latency_ms {snap['mean_latency_ms']:.4f}",
        "# HELP quasar_healthy Cluster health (1=ok)",
        "# TYPE quasar_healthy gauge",
        f"quasar_healthy {1 if health.get('healthy') else 0}",
    ]
    for name, ok in health.get("shards", {}).items():
        lines.append(f'quasar_shard_up{{shard="{name}"}} {1 if ok else 0}')
    return "\n".join(lines) + "\n"


def create_gateway_app(cluster: QuasarCluster, *, api_key: Optional[str] = None):
    try:
        from flask import Flask, jsonify, request
    except ImportError as exc:
        raise ImportError("Flask is required for the gateway: pip install flask") from exc

    policy = cluster.security
    effective_key = api_key or os.environ.get("QUASAR_API_KEY")
    if policy.require_gateway_auth and not effective_key:
        raise ValueError("gateway requires QUASAR_API_KEY or --api-key when security.require_gateway_auth is true")

    app = Flask(__name__)
    app.config["MAX_CONTENT_LENGTH"] = policy.max_gateway_body_bytes
    limiter = RateLimiter(policy.gateway_rate_per_minute)
    config_root = cluster.config_path.parent if cluster.config_path else Path.cwd()

    @app.errorhandler(Exception)
    def _handle_error(err):
        if isinstance(err, QuasarSecurityError):
            return jsonify({"error": "bad request"}), 400
        if isinstance(err, QuasarRestoreError):
            return jsonify({"error": "unsafe restore"}), 409
        if isinstance(err, QuasarOverloadError):
            resp = jsonify({"error": "overloaded"})
            resp.headers["Retry-After"] = "1"
            return resp, 503
        if isinstance(err, QuasarCircuitOpenError):
            resp = jsonify({"error": "shard unavailable"})
            resp.headers["Retry-After"] = "5"
            return resp, 503
        return jsonify({"error": "internal error"}), 500

    def _rate_limit() -> Optional[tuple]:
        client_ip = request.headers.get("X-Forwarded-For", request.remote_addr or "unknown").split(",")[0].strip()
        if not limiter.allow(client_ip):
            return jsonify({"error": "rate limit exceeded"}), 429
        return None

    def _check_auth() -> Optional[tuple]:
        if not effective_key:
            return None
        provided = request.headers.get("X-Quasar-Key") or request.args.get("api_key")
        if not constant_time_equal(provided, effective_key):
            return jsonify({"error": "unauthorized"}), 401
        return None

    @app.route("/health", methods=["GET"])
    def health():
        auth = _check_auth()
        if auth:
            return auth
        return jsonify(cluster.health())

    @app.route("/ready", methods=["GET"])
    def ready():
        auth = _check_auth()
        if auth:
            return auth
        report = cluster.health()
        code = 200 if report.get("healthy") else 503
        return jsonify({"ready": report.get("healthy", False)}), code

    @app.route("/metrics", methods=["GET"])
    def metrics():
        auth = _check_auth()
        if auth:
            return auth
        if request.args.get("format") == "prometheus":
            from flask import Response

            return Response(format_prometheus_metrics(cluster), mimetype="text/plain; version=0.0.4")
        payload = {
            "monitor": cluster.monitor.snapshot(),
            "pool": cluster.pool_stats(),
            "workload": cluster.workload_stats(),
        }
        return jsonify(payload)

    @app.route("/query", methods=["POST"])
    def query():
        blocked = _rate_limit() or _check_auth()
        if blocked:
            return blocked
        body = request.get_json(force=True, silent=True) or {}
        sql = body.get("sql")
        if not sql or not isinstance(sql, str):
            return jsonify({"error": "missing sql"}), 400
        shard_key = body.get("shard_key")
        if shard_key is not None and not isinstance(shard_key, str):
            return jsonify({"error": "invalid shard_key"}), 400
        merge = bool(body.get("merge"))
        try:
            if merge and shard_key is None:
                merged = cluster.execute_merged(sql, shard_key=None)
                return jsonify(merged.to_dict())
            results = cluster.execute(sql, shard_key=shard_key)
            return jsonify(
                [
                    {
                        "node": r.node,
                        "database": str(r.database),
                        "stdout": r.result.stdout,
                        "stderr": r.result.stderr,
                        "returncode": r.result.returncode,
                        "elapsed_ms": r.result.elapsed_ms,
                    }
                    for r in results
                ]
            )
        except QuasarSecurityError:
            return jsonify({"error": "bad request"}), 400
        except ValueError:
            return jsonify({"error": "merge not allowed for this SQL"}), 400
        except (RuntimeError, QuasarCircuitOpenError, QuasarOverloadError) as err:
            code = 503 if isinstance(err, (QuasarCircuitOpenError, QuasarOverloadError)) else 500
            resp = jsonify({"error": str(err)})
            if code == 503:
                resp.headers["Retry-After"] = "5" if isinstance(err, QuasarCircuitOpenError) else "1"
            return resp, code

    @app.route("/cross-join", methods=["POST"])
    def cross_join_route():
        blocked = _rate_limit() or _check_auth()
        if blocked:
            return blocked
        body = request.get_json(force=True, silent=True) or {}
        spec_path = body.get("spec_file")
        if spec_path:
            if not isinstance(spec_path, str):
                return jsonify({"error": "invalid spec_file"}), 400
            safe = resolve_path_under_base(
                config_root,
                spec_path,
                allow_outside=policy.allow_path_outside_config_root,
            )
            rows = cluster.cross_join(safe)
        elif body.get("spec"):
            from quasar.crossjoin import CrossJoinSpec, QuasarCrossShardJoin
            import tempfile

            tmp = Path(tempfile.gettempdir()) / "quasar_xj_spec.json"
            tmp.write_text(json.dumps(body["spec"]), encoding="utf-8")
            rows = QuasarCrossShardJoin(cluster.active_shard(), client=cluster.client).execute(
                CrossJoinSpec.from_dict(body["spec"])
            )
        else:
            return jsonify({"error": "spec or spec_file required"}), 400
        return jsonify({"rows": rows, "count": len(rows)})

    @app.route("/failover/run", methods=["POST"])
    def failover_run():
        auth = _check_auth()
        if auth:
            return auth
        return jsonify(cluster.failover_tick())

    @app.route("/dtxn/recover", methods=["POST"])
    def dtxn_recover_route():
        auth = _check_auth()
        if auth:
            return auth
        try:
            return jsonify({"recovered": cluster.dtxn_recover()})
        except RuntimeError as err:
            return jsonify({"error": str(err)}), 400

    @app.route("/recover", methods=["POST"])
    def recover_route():
        auth = _check_auth()
        if auth:
            return auth
        body = request.get_json(force=True, silent=True) or {}
        force = bool(body.get("force"))
        return jsonify(cluster.recovery_tick(force=force))

    @app.route("/consensus", methods=["GET", "POST"])
    def consensus_route():
        auth = _check_auth()
        if auth:
            return auth
        if request.method == "POST":
            body = request.get_json(force=True, silent=True) or {}
            if body.get("elect"):
                if cluster.consensus:
                    cluster.consensus.elect_leader()
            return jsonify(cluster.consensus_status())
        return jsonify(cluster.consensus_status())

    @app.route("/rebalance-auto", methods=["POST"])
    def rebalance_auto_route():
        auth = _check_auth()
        if auth:
            return auth
        body = request.get_json(force=True, silent=True) or {}
        apply = body.get("apply")
        return jsonify(cluster.rebalance_tick(force_apply=bool(apply) if apply is not None else None))

    @app.route("/autoscale", methods=["GET", "POST"])
    def autoscale_route():
        auth = _check_auth()
        if auth:
            return auth
        body = request.get_json(force=True, silent=True) or {} if request.method == "POST" else {}
        apply = body.get("apply") if request.method == "POST" else None
        if apply is not None:
            apply = bool(apply)
        return jsonify(cluster.autoscale_tick(apply=apply))

    @app.route("/cross-query", methods=["POST"])
    def cross_query_route():
        blocked = _rate_limit() or _check_auth()
        if blocked:
            return blocked
        body = request.get_json(force=True, silent=True) or {}
        try:
            from quasar.crossquery import CrossQuerySpec

            spec = CrossQuerySpec.from_dict(body)
            api = cluster.cross_query_api
            if api is None:
                from quasar.crossquery import QuasarCrossQuery

                api = QuasarCrossQuery(cluster)
            result = api.execute(spec)
            return jsonify(result.to_dict())
        except ValueError as err:
            return jsonify({"error": str(err)}), 400
        except (RuntimeError, QuasarCircuitOpenError, QuasarOverloadError) as err:
            code = 503 if isinstance(err, (QuasarCircuitOpenError, QuasarOverloadError)) else 500
            return jsonify({"error": str(err)}), code

    @app.route("/status", methods=["GET"])
    def status():
        auth = _check_auth()
        if auth:
            return auth
        hash_files = request.args.get("hash", "").lower() in ("1", "true", "yes")
        return jsonify(cluster.status(hash_files=hash_files))

    @app.route("/drift", methods=["GET"])
    def drift():
        auth = _check_auth()
        if auth:
            return auth
        probe_sql = request.args.get("probe_sql")
        use_hashes = request.args.get("no_hash", "").lower() not in ("1", "true", "yes")
        check_replicas = request.args.get("replicas", "").lower() in ("1", "true", "yes")
        reports = cluster.check_drift(
            probe_sql=probe_sql,
            use_hashes=use_hashes,
            check_replicas=check_replicas,
        )
        consistent = all(r["consistent"] for r in reports) if reports else True
        return jsonify({"consistent": consistent, "reports": reports}), 200 if consistent else 409

    @app.route("/shards", methods=["GET"])
    def shards():
        auth = _check_auth()
        if auth:
            return auth
        return jsonify(
            [
                {"name": n.name, "database": str(n.database), "wal": str(n.wal_path())}
                for n in cluster.shard.nodes
            ]
        )

    @app.route("/ring", methods=["GET"])
    def ring():
        auth = _check_auth()
        if auth:
            return auth
        key = request.args.get("key")
        if not key:
            return jsonify({"error": "missing key query param"}), 400
        node = cluster.shard.node_for_key(key)
        return jsonify({"shard_key": key, "node": node.name, "database": str(node.database)})

    @app.route("/transfer", methods=["POST"])
    def transfer_route():
        blocked = _rate_limit() or _check_auth()
        if blocked:
            return blocked
        body = request.get_json(force=True, silent=True) or {}
        try:
            result = cluster.transfer(body)
            code = 200 if result.status == "posted" else 409
            return jsonify(result.to_dict()), code
        except RuntimeError as err:
            return jsonify({"error": str(err)}), 400

    @app.route("/saga", methods=["POST"])
    def saga_route():
        blocked = _rate_limit() or _check_auth()
        if blocked:
            return blocked
        body = request.get_json(force=True, silent=True) or {}
        try:
            result = cluster.run_saga(body)
            code = 200 if result.status == "completed" else 409
            return jsonify(result.to_dict()), code
        except RuntimeError as err:
            return jsonify({"error": str(err)}), 400

    @app.route("/reconcile", methods=["GET"])
    def reconcile_route():
        auth = _check_auth()
        if auth:
            return auth
        probe = request.args.get("probe_sql")
        try:
            report = cluster.reconcile(probe_sql=probe)
            code = 200 if report.consistent else 409
            return jsonify(report.to_dict()), code
        except RuntimeError as err:
            return jsonify({"error": str(err)}), 400

    @app.route("/batch", methods=["POST"])
    def batch():
        auth = _check_auth()
        if auth:
            return auth
        body = request.get_json(force=True, silent=True) or {}
        items = body.get("statements", [])
        if not isinstance(items, list):
            return jsonify({"error": "statements must be an array"}), 400
        out = []
        for item in items:
            sql = item.get("sql")
            if not sql:
                continue
            try:
                results = cluster.execute(sql, shard_key=item.get("shard_key"))
                out.append(
                    {
                        "sql": sql[:120],
                        "results": [
                            {"node": r.node, "ok": r.result.ok, "elapsed_ms": r.result.elapsed_ms}
                            for r in results
                        ],
                    }
                )
            except RuntimeError as err:
                out.append({"sql": sql[:120], "error": str(err)})
        return jsonify(out)

    return app


def _load_json_config(path: PathLike) -> Dict[str, Any]:
    return load_cluster_config(path)


def scaffold_cluster(
    root: PathLike,
    *,
    shard_count: int = 3,
    force: bool = False,
) -> Path:
    """Create data/, backups/, and a starter cluster.json under root."""
    root = Path(root)
    config_path = root / "cluster.json"
    if config_path.exists() and not force:
        raise FileExistsError(f"already exists: {config_path} (use force=True)")

    (root / "data").mkdir(parents=True, exist_ok=True)
    (root / "backups").mkdir(parents=True, exist_ok=True)
    template = default_cluster_template(shard_count)
    config_path.write_text(json.dumps(template, indent=2) + "\n", encoding="utf-8")
    return config_path


def _build_client(args: argparse.Namespace) -> AstralDBClient:
    exe = getattr(args, "astraldb", None)
    return AstralDBClient(
        executable=exe,
        user=getattr(args, "user", None),
        password=getattr(args, "password", None),
        optimization=getattr(args, "optimization", "O2") or "O2",
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Quasar — AstralDB orchestration (sharding, replication, backup, migration)",
    )
    parser.add_argument(
        "--astraldb",
        type=Path,
        help="Path to astraldb executable (or set QUASAR_ASTRALDB)",
    )
    parser.add_argument("-U", "--user", help="AstralDB user (-U)")
    parser.add_argument("-P", "--password", help="AstralDB password (-P)")
    parser.add_argument("-O", "--optimization", default="O2", help="Optimization level (default O2)")

    sub = parser.add_subparsers(dest="command", required=True)

    p_ver = sub.add_parser("version", help="Print Quasar and AstralDB versions")
    p_ver.set_defaults(func=_cmd_version)

    p_init = sub.add_parser("init", help="Scaffold cluster directories and cluster.json")
    p_init.add_argument("root", type=Path, nargs="?", default=".", help="Project root")
    p_init.add_argument("--shards", type=int, default=3, help="Number of shards")
    p_init.add_argument("--force", action="store_true", help="Overwrite existing cluster.json")
    p_init.set_defaults(func=_cmd_init)

    p_health = sub.add_parser("health", help="Health-check databases from config")
    p_health.add_argument("config", type=Path, help="Cluster JSON config")
    p_health.add_argument("--json", action="store_true", help="Emit JSON only (no extra output)")
    p_health.set_defaults(func=_cmd_health)

    p_query = sub.add_parser("query", help="Run SQL against a single database file")
    p_query.add_argument("database", type=Path)
    p_query.add_argument("sql")
    p_query.set_defaults(func=_cmd_query)

    p_shard = sub.add_parser("shard", help="Run SQL on a sharded cluster")
    p_shard.add_argument("config", type=Path, help="Shard JSON config")
    p_shard.add_argument("sql")
    p_shard.add_argument("--shard-key", help="Route to one shard")
    p_shard.add_argument("--multi-master", action="store_true", help="Quorum write to all masters")
    p_shard.add_argument(
        "--merge",
        action="store_true",
        help="Broadcast read: merge per-shard stdout into one stream",
    )
    p_shard.set_defaults(func=_cmd_shard)

    p_script = sub.add_parser("script", help="Run a .sql file on a cluster or single DB")
    p_script.add_argument("script", type=Path)
    p_script.add_argument("--database", type=Path, help="Single database file")
    p_script.add_argument("--config", type=Path, help="Cluster JSON (fan-out to all shards)")
    p_script.add_argument("--shard-key", help="Route to one shard")
    p_script.set_defaults(func=_cmd_script)

    p_rep = sub.add_parser("replicate", help="Execute SQL on master and replicas")
    p_rep.add_argument("master", type=Path)
    p_rep.add_argument("replicas", type=Path, nargs="*")
    p_rep.add_argument("sql")
    p_rep.add_argument("--parallel", action="store_true", help="Write to all nodes in parallel")
    p_rep.set_defaults(func=_cmd_replicate)

    p_backup = sub.add_parser("backup", help="Backup an AstralDB file")
    p_backup.add_argument("database", type=Path)
    p_backup.add_argument("backup_dir", type=Path)
    p_backup.add_argument("--version", help="Backup label")
    p_backup.add_argument("--incremental", action="store_true", help="WAL-only incremental backup")
    p_backup.set_defaults(func=_cmd_backup)

    p_restore = sub.add_parser("restore", help="Restore a backup version")
    p_restore.add_argument("backup_dir", type=Path)
    p_restore.add_argument("version", help="Backup folder name")
    p_restore.add_argument("target_db", type=Path)
    p_restore.add_argument("--overwrite", action="store_true")
    p_restore.add_argument(
        "--force",
        action="store_true",
        help="Restore even if WAL was modified recently (active writer risk)",
    )
    p_restore.set_defaults(func=_cmd_restore)

    p_blist = sub.add_parser("backup-list", help="List backup versions")
    p_blist.add_argument("backup_dir", type=Path)
    p_blist.add_argument("--json", action="store_true")
    p_blist.set_defaults(func=_cmd_backup_list)

    p_bprune = sub.add_parser("backup-prune", help="Prune old backups")
    p_bprune.add_argument("backup_dir", type=Path)
    p_bprune.add_argument("--keep-last", type=int, help="Retain N newest versions")
    p_bprune.add_argument("--keep-days", type=float, help="Drop backups older than N days")
    p_bprune.add_argument("--dry-run", action="store_true")
    p_bprune.set_defaults(func=_cmd_backup_prune)

    p_ball = sub.add_parser("backup-all", help="Backup every shard in a cluster config")
    p_ball.add_argument("config", type=Path)
    p_ball.add_argument("--incremental", action="store_true")
    p_ball.add_argument("--prefix", default="", help="Version label prefix")
    p_ball.set_defaults(func=_cmd_backup_all)

    p_mig = sub.add_parser("migrate", help="Migrate source DB to target")
    p_mig.add_argument("source", type=Path)
    p_mig.add_argument("target", type=Path)
    p_mig.add_argument("--copy", action="store_true", help="File copy instead of bundle export/import")
    p_mig.add_argument("--work-dir", type=Path)
    p_mig.set_defaults(func=_cmd_migrate)

    p_gw = sub.add_parser("gateway", help="Start HTTP gateway (requires flask)")
    p_gw.add_argument("config", type=Path)
    p_gw.add_argument("--host", default="127.0.0.1")
    p_gw.add_argument("--port", type=int, default=8080)
    p_gw.add_argument("--api-key", help="Require X-Quasar-Key header (or ?api_key=)")
    p_gw.add_argument(
        "--require-auth",
        action="store_true",
        help="Reject requests without valid API key (or set security.require_gateway_auth)",
    )
    p_gw.set_defaults(func=_cmd_gateway)

    p_validate = sub.add_parser("validate", help="Validate cluster JSON config")
    p_validate.add_argument("config", type=Path)
    p_validate.set_defaults(func=_cmd_validate)

    p_status = sub.add_parser("status", help="Health + disk inventory for cluster")
    p_status.add_argument("config", type=Path)
    p_status.add_argument("--hash", action="store_true", help="Include SHA-256 per shard file")
    p_status.set_defaults(func=_cmd_status)

    p_drift = sub.add_parser("drift", help="Detect inconsistent shards")
    p_drift.add_argument("config", type=Path)
    p_drift.add_argument("--probe-sql", help="SQL run on each shard; compare stdout")
    p_drift.add_argument("--no-hash", action="store_true", help="Skip file hash comparison")
    p_drift.add_argument("--replicas", action="store_true", help="Compare replica files to masters")
    p_drift.set_defaults(func=_cmd_drift)

    p_ring = sub.add_parser("ring", help="Show shard routing for sample keys")
    p_ring.add_argument("config", type=Path)
    p_ring.add_argument("keys", nargs="+", help="Sample shard keys")
    p_ring.set_defaults(func=_cmd_ring)

    p_roll = sub.add_parser("rolling", help="Apply SQL to shards sequentially")
    p_roll.add_argument("config", type=Path)
    p_roll.add_argument("sql")
    p_roll.add_argument("--delay", type=float, default=0.0, help="Seconds between shards")
    p_roll.set_defaults(func=_cmd_rolling)

    p_batch = sub.add_parser("batch", help="Run @shard / @broadcast annotated SQL file")
    p_batch.add_argument("config", type=Path)
    p_batch.add_argument("script", type=Path)
    p_batch.set_defaults(func=_cmd_batch)

    p_watch = sub.add_parser("watch", help="Periodic health (+ optional backup) loop")
    p_watch.add_argument("config", type=Path)
    p_watch.add_argument("--interval", type=float, default=60.0)
    p_watch.add_argument("--iterations", type=int, default=0, help="0 = run forever")
    p_watch.add_argument("--backup", action="store_true")
    p_watch.add_argument("--incremental", action="store_true")
    p_watch.add_argument("--failover", action="store_true", help="Run failover tick each interval")
    p_watch.add_argument("--drift", action="store_true", help="Run drift check each tick")
    p_watch.add_argument("--probe-sql", help="Drift probe SQL (with --drift)")
    p_watch.add_argument("--replicas", action="store_true", help="Include replica hash drift in --drift")
    p_watch.add_argument("--recover", action="store_true", help="Run crash recovery each tick")
    p_watch.add_argument("--autoscale", action="store_true", help="Evaluate autoscale metrics each tick")
    p_watch.add_argument(
        "--autoscale-apply",
        action="store_true",
        help="Apply scale-out when recommended (or use autoscaling.auto_apply)",
    )
    p_watch.add_argument("--rebalance-auto", action="store_true", help="Run automatic rebalance each tick")
    p_watch.add_argument(
        "--rebalance-apply",
        action="store_true",
        help="Apply rebalance moves (or use rebalance.automation.auto_apply)",
    )
    p_watch.set_defaults(func=_cmd_watch)

    p_as = sub.add_parser("autoscale", help="Shard autoscaling (evaluate or apply scale-out)")
    p_as.add_argument("config", type=Path)
    p_as.add_argument("--apply", action="store_true", help="Add shard(s) when scale-out is recommended")
    p_as.add_argument("--metrics", action="store_true", help="Print metrics only (no decision)")
    p_as.set_defaults(func=_cmd_autoscale)

    p_cons = sub.add_parser("consensus", help="Distributed consensus status and leader election")
    p_cons.add_argument("config", type=Path)
    p_cons.add_argument("--elect", action="store_true", help="Force leader election for this node")
    p_cons.set_defaults(func=_cmd_consensus)

    p_rba = sub.add_parser("rebalance-auto", help="Automatic rebalance plan/apply for current ring")
    p_rba.add_argument("config", type=Path)
    p_rba.add_argument("--apply", action="store_true", help="Apply moves when plan is non-empty")
    p_rba.set_defaults(func=_cmd_rebalance_auto)

    p_repair = sub.add_parser("repair-replica", help="Copy master DB to replica files")
    p_repair.add_argument("config", type=Path)
    p_repair.add_argument("replica_set", help="Name from config replicas.{name}")
    p_repair.set_defaults(func=_cmd_repair_replica)

    p_compile = sub.add_parser("compile", help="Compile SQL to .abc bytecode")
    p_compile.add_argument("sql", type=Path)
    p_compile.add_argument("-o", "--output", type=Path, default="out.abc")
    p_compile.add_argument("--pool", action="store_true", help="Emit string pool (-cp)")
    p_compile.set_defaults(func=_cmd_compile)

    p_fo = sub.add_parser("failover", help="Automatic failover control")
    p_fo.add_argument("config", type=Path)
    p_fo_sub = p_fo.add_subparsers(dest="failover_cmd", required=True)
    p_fo_status = p_fo_sub.add_parser("status")
    p_fo_status.set_defaults(func=_cmd_failover_status)
    p_fo_run = p_fo_sub.add_parser("run", help="Run one failover tick")
    p_fo_run.set_defaults(func=_cmd_failover_run)
    p_fo_reset = p_fo_sub.add_parser("reset")
    p_fo_reset.set_defaults(func=_cmd_failover_reset)
    p_fo_failback = p_fo_sub.add_parser("fail-back", help="Route shard back to primary when healthy")
    p_fo_failback.add_argument("shard", help="Shard name")
    p_fo_failback.set_defaults(func=_cmd_failover_failback)

    p_rb = sub.add_parser("rebalance", help="Plan/apply shard rebalancing")
    p_rb.add_argument("config", type=Path)
    p_rb.add_argument("new_shards", nargs="+", help="Target shard names for new ring")
    p_rb.add_argument("--apply", action="store_true", help="Execute planned moves")
    p_rb.add_argument("--delete-source", action="store_true")
    p_rb.add_argument("--plan-file", type=Path, help="Apply saved plan JSON instead of planning")
    p_rb.set_defaults(func=_cmd_rebalance)

    p_xj = sub.add_parser("cross-join", help="Cross-shard JOIN via bundle export")
    p_xj.add_argument("config", type=Path)
    p_xj.add_argument("spec", type=Path, help="Join spec JSON")
    p_xj.add_argument("-o", "--output", type=Path, help="Write results JSON to file")
    p_xj.set_defaults(func=_cmd_cross_join)

    p_mm = sub.add_parser("multi-master", help="Multi-master write/read")
    p_mm.add_argument("config", type=Path)
    p_mm.add_argument("sql")
    p_mm.add_argument("--shard-key", required=True)
    p_mm.add_argument("--read", action="store_true", help="Read instead of write")
    p_mm.set_defaults(func=_cmd_multi_master)

    p_pool = sub.add_parser("pool-stats", help="Connection pool / batching statistics")
    p_pool.add_argument("config", type=Path)
    p_pool.set_defaults(func=_cmd_pool_stats)

    p_wl = sub.add_parser("workload-stats", help="Circuit breaker and workload counters")
    p_wl.add_argument("config", type=Path)
    p_wl.set_defaults(func=_cmd_workload_stats)

    p_xtxn = sub.add_parser("xtxn", help="Cross-shard ACID transaction (coordinator 2PC)")
    p_xtxn.add_argument("config", type=Path)
    p_xtxn.add_argument("--spec", type=Path, help='JSON: {"statements":[{"sql":"…","shard_key":"…"}]}')
    p_xtxn.add_argument("--all-shards", action="store_true", help="Begin on every shard (legacy mode)")
    p_xtxn.set_defaults(func=_cmd_xtxn)

    p_dtxn_rec = sub.add_parser("dtxn-recover", help="Abort in-doubt distributed transactions")
    p_dtxn_rec.add_argument("config", type=Path)
    p_dtxn_rec.set_defaults(func=_cmd_dtxn_recover)

    p_recover = sub.add_parser("recover", help="Automated crash recovery (dtxn + rollbacks + pool heal)")
    p_recover.add_argument("config", type=Path)
    p_recover.add_argument("--force", action="store_true", help="Run even when recovery_automation is disabled")
    p_recover.set_defaults(func=_cmd_recover)

    p_xq = sub.add_parser("cross-query", help="Cross-shard fan-out or scatter-gather reads")
    p_xq.add_argument("config", type=Path)
    p_xq.add_argument("sql", nargs="?", help="SQL to fan out to all shards")
    p_xq.add_argument("--merge", action="store_true", help="Merge read results (like shard --merge)")
    p_xq.add_argument("--spec", type=Path, help="JSON: {sql} or {queries:[{sql,shard_key}]}")
    p_xq.add_argument("--no-parallel", action="store_true", help="Run shard queries sequentially")
    p_xq.set_defaults(func=_cmd_cross_query)

    p_reg = sub.add_parser("region", help="Multi-region read/write")
    p_reg.add_argument("config", type=Path)
    p_reg.add_argument("sql")
    p_reg.add_argument("--shard-key", required=True)
    p_reg.add_argument("--read", action="store_true")
    p_reg.add_argument("--region", help="Target region name for reads")
    p_reg.add_argument("--health", action="store_true", help="Print region health only")
    p_reg.set_defaults(func=_cmd_region)

    p_saga = sub.add_parser("saga", help="Run compensating saga from JSON spec")
    p_saga.add_argument("config", type=Path)
    p_saga.add_argument("spec", type=Path)
    p_saga.set_defaults(func=_cmd_saga)

    p_xfer = sub.add_parser("transfer", help="Cross-shard ledger transfer (saga-backed)")
    p_xfer.add_argument("config", type=Path)
    p_xfer.add_argument("spec", type=Path, help="Transfer JSON spec")
    p_xfer.set_defaults(func=_cmd_transfer)

    p_recon = sub.add_parser("reconcile", help="Cross-shard balance / probe reconciliation")
    p_recon.add_argument("config", type=Path)
    p_recon.add_argument("--probe-sql", help="Custom reconciliation SQL")
    p_recon.set_defaults(func=_cmd_reconcile)

    p_fin_init = sub.add_parser("financial-init", help="Bootstrap accounts/ledger schema on all shards")
    p_fin_init.add_argument("config", type=Path)
    p_fin_init.set_defaults(func=_cmd_financial_init)

    p_journal = sub.add_parser("journal", help="Tail Quasar operation journal")
    p_journal.add_argument("config", type=Path)
    p_journal.add_argument("--limit", type=int, default=50)
    p_journal.set_defaults(func=_cmd_journal)

    args = parser.parse_args(argv)
    return int(args.func(args))


def _cmd_version(args: argparse.Namespace) -> int:
    client = _build_client(args)
    print(f"Quasar {QUASAR_VERSION}")
    print(client.version())
    return 0


def _cmd_init(args: argparse.Namespace) -> int:
    path = scaffold_cluster(args.root, shard_count=args.shards, force=args.force)
    print(path)
    return 0


def _cmd_health(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    report = cluster.health()
    print(json.dumps(report, indent=2))
    return 0 if report.get("healthy") else 1


def _cmd_validate(args: argparse.Namespace) -> int:
    try:
        cfg = load_cluster_config(args.config)
        print(json.dumps({"valid": True, "shards": len(cfg["shards"]), "path": str(args.config)}))
        return 0
    except ConfigError as err:
        print(json.dumps({"valid": False, "error": str(err)}))
        return 1


def _cmd_query(args: argparse.Namespace) -> int:
    client = _build_client(args)
    result = client.query(args.sql, database=args.database)
    sys.stdout.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)
    return 0


def _cmd_shard(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if getattr(args, "multi_master", False) and args.shard_key:
        for routed in cluster.execute(args.sql, shard_key=args.shard_key, multi_master=True):
            print(f"--- {routed.node} ({routed.database}) ---")
            sys.stdout.write(routed.result.stdout)
        return 0
    if getattr(args, "merge", False) and not args.shard_key:
        merged = cluster.execute_merged(args.sql)
        sys.stdout.write(merged.merged_stdout)
        return 0 if merged.all_ok else 1
    shard = cluster.active_shard()
    for routed in shard.execute(args.sql, shard_key=args.shard_key):
        print(f"--- {routed.node} ({routed.database}) ---")
        sys.stdout.write(routed.result.stdout)
    return 0


def _cmd_script(args: argparse.Namespace) -> int:
    client = _build_client(args)
    if args.database:
        result = client.script(args.script, database=args.database)
        sys.stdout.write(result.stdout)
        return 0
    if not getattr(args, "config", None):
        print("error: pass --database or --config", file=sys.stderr)
        return 2
    cluster = QuasarCluster.from_file(args.config, client=client)
    for routed in cluster.run_script(args.script, shard_key=args.shard_key):
        print(f"--- {routed.node} ({routed.database}) ---")
        sys.stdout.write(routed.result.stdout)
    return 0


def _cmd_replicate(args: argparse.Namespace) -> int:
    rep = QuasarReplica(
        ReplicaSet(master=args.master, replicas=list(args.replicas)),
        client=_build_client(args),
    )
    results = rep.write_parallel(args.sql) if args.parallel else rep.write(args.sql)
    for i, r in enumerate(results):
        label = "master" if i == 0 else f"replica_{i}"
        print(f"--- {label} ---")
        sys.stdout.write(r.stdout)
    return 0


def _cmd_backup(args: argparse.Namespace) -> int:
    backup = QuasarBackup(args.backup_dir)
    if args.incremental:
        dest = backup.incremental_backup(args.database, version=args.version)
        if dest is None:
            print("No WAL to backup (skipped).")
            return 0
    else:
        dest = backup.backup(args.database, version=args.version)
    print(str(dest))
    return 0


def _cmd_restore(args: argparse.Namespace) -> int:
    backup = QuasarBackup(args.backup_dir)
    try:
        path = backup.restore(
            args.version,
            args.target_db,
            overwrite=args.overwrite,
            force=getattr(args, "force", False),
        )
    except QuasarRestoreError as err:
        print(str(err), file=sys.stderr)
        return 1
    print(str(path))
    return 0


def _cmd_backup_list(args: argparse.Namespace) -> int:
    backup = QuasarBackup(args.backup_dir)
    versions = backup.list_versions()
    if args.json:
        details = []
        for v in versions:
            try:
                m = backup.describe_version(v)
                details.append(asdict(m))
            except FileNotFoundError:
                details.append({"version": v})
        print(json.dumps(details, indent=2))
    else:
        for v in versions:
            print(v)
    return 0


def _cmd_backup_prune(args: argparse.Namespace) -> int:
    backup = QuasarBackup(args.backup_dir)
    removed = backup.prune(keep_last=args.keep_last, keep_days=args.keep_days, dry_run=args.dry_run)
    prefix = "would remove" if args.dry_run else "removed"
    for v in removed:
        print(f"{prefix}: {v}")
    return 0


def _cmd_backup_all(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    results = cluster.backup_all(incremental=args.incremental, label_prefix=args.prefix)
    print(json.dumps({k: str(v) if v else None for k, v in results.items()}, indent=2))
    return 0


def _cmd_migrate(args: argparse.Namespace) -> int:
    mig = QuasarMigration(args.source, args.target, client=_build_client(args))
    if args.copy:
        mig.migrate_via_checkpoint_copy()
        print(f"Copied to {args.target}")
    else:
        bundle = mig.migrate_via_bundle(work_dir=args.work_dir)
        print(f"Migrated via {bundle}")
    return 0


def _cmd_gateway(args: argparse.Namespace) -> int:
    from dataclasses import replace

    api_key = args.api_key or os.environ.get("QUASAR_API_KEY")
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if args.require_auth:
        cluster.security = replace(cluster.security, require_gateway_auth=True)
    app = create_gateway_app(cluster, api_key=api_key)
    try:
        app.run(host=args.host, port=args.port)
    finally:
        cluster.close()
    return 0


def _cmd_status(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    print(json.dumps(cluster.status(hash_files=args.hash), indent=2))
    return 0


def _cmd_drift(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    reports = cluster.check_drift(
        probe_sql=args.probe_sql,
        use_hashes=not args.no_hash,
        check_replicas=getattr(args, "replicas", False),
    )
    print(json.dumps(reports, indent=2))
    consistent = all(r["consistent"] for r in reports) if reports else True
    return 0 if consistent else 1


def _cmd_ring(args: argparse.Namespace) -> int:
    from quasar.ops import shard_ring_map

    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    mapping = shard_ring_map(cluster.shard, args.keys)
    print(json.dumps(mapping, indent=2))
    return 0


def _cmd_rolling(args: argparse.Namespace) -> int:
    from quasar.ops import QuasarRolling

    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    rolling = QuasarRolling(cluster.shard)
    for routed in rolling.execute(args.sql, delay_sec=args.delay):
        print(f"--- {routed.node} ---")
        sys.stdout.write(routed.result.stdout)
    return 0


def _cmd_batch(args: argparse.Namespace) -> int:
    from quasar.sqlbatch import QuasarSqlBatch

    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    batch = QuasarSqlBatch(cluster.shard)
    print(json.dumps(batch.run_file(args.script), indent=2))
    return 0


def _cmd_watch(args: argparse.Namespace) -> int:
    from quasar.ops import QuasarWatch

    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    watch = QuasarWatch(cluster)
    iterations = None if args.iterations == 0 else args.iterations

    def _print_tick(snap: Dict[str, Any]) -> None:
        print(json.dumps(snap, indent=2), flush=True)

    watch.run(
        args.interval,
        iterations=iterations,
        backup=args.backup,
        incremental=args.incremental,
        failover=getattr(args, "failover", False),
        drift=getattr(args, "drift", False),
        probe_sql=getattr(args, "probe_sql", None),
        check_replicas=getattr(args, "replicas", False),
        recover=getattr(args, "recover", False),
        autoscale=getattr(args, "autoscale", False),
        autoscale_apply=getattr(args, "autoscale_apply", False),
        rebalance_auto=getattr(args, "rebalance_auto", False),
        rebalance_apply=getattr(args, "rebalance_apply", False),
        on_tick=_print_tick,
    )
    return 0


def _cmd_consensus(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if getattr(args, "elect", False) and cluster.consensus:
        cluster.consensus.elect_leader()
    print(json.dumps(cluster.consensus_status(), indent=2))
    return 0


def _cmd_rebalance_auto(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    apply = True if getattr(args, "apply", False) else None
    report = cluster.rebalance_tick(force_apply=apply)
    print(json.dumps(report, indent=2))
    return 0 if report.get("action") != "blocked" else 1


def _cmd_autoscale(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if getattr(args, "metrics", False):
        from quasar.autoscale import AutoscaleConfig, QuasarAutoscale

        mgr = cluster.autoscale or QuasarAutoscale(cluster, AutoscaleConfig(enabled=True))
        print(json.dumps(mgr.collect_metrics().to_dict(), indent=2))
        return 0
    apply = True if getattr(args, "apply", False) else None
    report = cluster.autoscale_tick(apply=apply)
    print(json.dumps(report, indent=2))
    action = report.get("decision", {}).get("action", "none")
    return 0 if action in ("none", "scale_out", "blocked") else 1


def _cmd_recover(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    report = cluster.recovery_tick(force=getattr(args, "force", False))
    print(json.dumps(report, indent=2))
    return 0 if report.get("ok") or report.get("skipped") else 1


def _cmd_cross_query(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    parallel = not getattr(args, "no_parallel", False)
    if args.spec:
        spec = json.loads(args.spec.read_text(encoding="utf-8"))
        if spec.get("queries"):
            result = cluster.cross_query(queries=spec["queries"], parallel=parallel)
        elif spec.get("sql"):
            result = cluster.cross_query(
                spec["sql"], merge=bool(spec.get("merge", args.merge)), parallel=parallel
            )
        else:
            print(json.dumps({"error": "spec needs sql or queries"}))
            return 1
    elif args.sql:
        result = cluster.cross_query(args.sql, merge=args.merge, parallel=parallel)
    else:
        print(json.dumps({"error": "sql or --spec required"}))
        return 1
    print(json.dumps(result.to_dict(), indent=2))
    return 0 if result.all_ok else 1


def _cmd_repair_replica(args: argparse.Namespace) -> int:
    from quasar.ops import repair_replica_set

    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    rep = cluster.replicas.get(args.replica_set)
    if rep is None:
        print(json.dumps({"error": f"unknown replica set: {args.replica_set}"}))
        return 1
    paths = repair_replica_set(rep)
    print(json.dumps({"repaired": [str(p) for p in paths]}))
    return 0


def _cmd_compile(args: argparse.Namespace) -> int:
    client = _build_client(args)
    client.compile_sql(args.sql, args.output, compile_pool=args.pool)
    print(str(args.output))
    return 0


def _cmd_failover_status(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if cluster.failover is None:
        print(json.dumps({"enabled": False}))
        return 0
    print(json.dumps(cluster.failover.health_report(), indent=2))
    return 0


def _cmd_failover_run(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if cluster.failover is None:
        print(json.dumps({"error": "failover not enabled in config"}))
        return 1
    actions = cluster.failover_tick()
    print(json.dumps(actions, indent=2))
    return 0 if not any("error" in a for a in actions) else 1


def _cmd_failover_reset(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if cluster.failover is None:
        return 1
    cluster.failover.reset()
    return 0


def _cmd_failover_failback(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if cluster.failover is None:
        print(json.dumps({"error": "failover not enabled"}))
        return 1
    path = cluster.failover.fail_back(args.shard)
    if path is None:
        print(json.dumps({"shard": args.shard, "fail_back": False}))
        return 1
    print(json.dumps({"shard": args.shard, "fail_back": str(path)}))
    return 0


def _cmd_dtxn_recover(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    actions = cluster.dtxn_recover()
    print(json.dumps({"recovered": actions}, indent=2))
    return 0


def _cmd_rebalance(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if args.plan_file:
        plan_dict = json.loads(args.plan_file.read_text(encoding="utf-8"))
        result = cluster.rebalance_apply(
            plan_dict, delete_from_source=args.delete_source
        )
        print(json.dumps(result, indent=2))
        return 0
    plan = cluster.rebalance.plan(args.new_shards)
    plan_dict = plan.to_dict()
    print(json.dumps(plan_dict, indent=2))
    if args.apply and plan.move_count > 0:
        full_plan = cluster.rebalance.plan(args.new_shards)
        apply_result = cluster.rebalance.apply(
            full_plan, delete_from_source=args.delete_source
        )
        print(json.dumps({"apply": apply_result}, indent=2))
    return 0


def _cmd_cross_join(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    try:
        rows = cluster.cross_join(args.spec)
    finally:
        cluster.close()
    payload = json.dumps({"count": len(rows), "rows": rows}, indent=2)
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
        print(str(args.output))
    else:
        print(payload)
    return 0


def _cmd_multi_master(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if cluster.multimaster is None:
        print(json.dumps({"error": "multi_master not configured"}))
        return 1
    if args.read:
        result = cluster.multimaster.read(args.sql, shard_key=args.shard_key)
        sys.stdout.write(result.stdout)
        return 0
    mm = cluster.multimaster.write(args.sql, shard_key=args.shard_key)
    print(json.dumps({"shard": mm.shard, "quorum_met": mm.quorum_met, "writers": len(mm.results)}))
    return 0


def _cmd_pool_stats(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    print(json.dumps(cluster.pool_stats(), indent=2))
    return 0


def _cmd_workload_stats(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    print(json.dumps(cluster.workload_stats(), indent=2))
    return 0


def _cmd_xtxn(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if args.spec:
        spec = json.loads(args.spec.read_text(encoding="utf-8"))
        result = cluster.cross_shard_transaction(spec.get("statements", []))
        print(json.dumps(result.to_dict(), indent=2))
        return 0 if result.committed else 1
    from quasar.xtxn import CrossShardTransaction

    txn = CrossShardTransaction(
        cluster.active_shard(),
        client=cluster.client,
        participant_names=set(n.name for n in cluster.shard.nodes) if args.all_shards else None,
    )
    with txn:
        print(json.dumps({"participants": [n.name for n in txn._participants]}))
    return 0


def _cmd_region(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if cluster.regions is None:
        print(json.dumps({"error": "regions not configured"}))
        return 1
    if args.health:
        print(json.dumps(cluster.regions.health(), indent=2))
        return 0
    if args.read:
        result = cluster.region_read(args.sql, shard_key=args.shard_key, region=args.region)
        sys.stdout.write(result.stdout)
        return 0
    wr = cluster.region_write(args.sql, shard_key=args.shard_key)
    print(
        json.dumps(
            {
                "region": wr.region,
                "shard": wr.shard,
                "replicated_to": wr.replicated_to,
            },
            indent=2,
        )
    )
    return 0


def _cmd_saga(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    spec = json.loads(args.spec.read_text(encoding="utf-8"))
    result = cluster.run_saga(spec)
    print(json.dumps(result.to_dict(), indent=2))
    return 0 if result.status == "completed" else 1


def _cmd_transfer(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    spec = json.loads(args.spec.read_text(encoding="utf-8"))
    result = cluster.transfer(spec)
    print(json.dumps(result.to_dict(), indent=2))
    return 0 if result.status == "posted" else 1


def _cmd_reconcile(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    report = cluster.reconcile(probe_sql=args.probe_sql)
    print(json.dumps(report.to_dict(), indent=2))
    return 0 if report.consistent else 1


def _cmd_financial_init(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if cluster.ledger is None:
        cluster.config.setdefault("financial", {})["enabled"] = True
        cluster._build_financial()
    if cluster.ledger is None:
        print(json.dumps({"error": "set financial.enabled in cluster config"}))
        return 1
    cluster.ledger.bootstrap()
    print(json.dumps({"ok": True, "schema": "accounts,ledger_entries"}))
    return 0


def _cmd_journal(args: argparse.Namespace) -> int:
    cluster = QuasarCluster.from_file(args.config, client=_build_client(args))
    if cluster.journal is None:
        print(json.dumps({"error": "financial not enabled"}))
        return 1
    print(json.dumps(cluster.journal.tail(args.limit), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
