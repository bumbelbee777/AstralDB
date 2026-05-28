"""Higher-level cluster operations: rolling deploys, watch loop, ring map, repair."""

from __future__ import annotations

import shutil
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from quasar.quasar import QuasarCluster, QuasarReplica, QuasarShard, RoutedResult


class QuasarRolling:
    """Apply SQL to shards one at a time (safer schema migrations)."""

    def __init__(self, shard: QuasarShard) -> None:
        self.shard = shard

    def execute(
        self,
        sql: str,
        *,
        delay_sec: float = 0.0,
        stop_on_error: bool = True,
    ) -> List[RoutedResult]:
        results: List[RoutedResult] = []
        for node in self.shard.nodes:
            try:
                result = self.shard.client.query(sql, database=node.database)
                results.append(RoutedResult(node=node.name, database=node.database, result=result))
            except Exception:
                if stop_on_error:
                    raise
            if delay_sec > 0:
                time.sleep(delay_sec)
        return results


class QuasarWatch:
    """Periodic health checks and optional backups (cron-friendly or long-running)."""

    def __init__(self, cluster: QuasarCluster) -> None:
        self.cluster = cluster

    def tick(
        self,
        *,
        backup: bool = False,
        incremental: bool = False,
        failover: bool = False,
        drift: bool = False,
        probe_sql: Optional[str] = None,
        check_replicas: bool = False,
        recover: bool = False,
        autoscale: bool = False,
        autoscale_apply: bool = False,
        rebalance_auto: bool = False,
        rebalance_apply: bool = False,
        matview_tick: bool = False,
        matview_force: bool = False,
    ) -> Dict[str, Any]:
        report = self.cluster.health()
        backup_result = None
        if backup and self.cluster.backup:
            backup_result = self.cluster.backup_all(incremental=incremental)
        failover_actions = self.cluster.failover_tick() if failover else None
        drift_reports = None
        if drift:
            drift_reports = self.cluster.check_drift(
                probe_sql=probe_sql,
                check_replicas=check_replicas,
            )
        recovery_report = self.cluster.recovery_tick() if recover else None
        autoscale_report = None
        if autoscale:
            autoscale_report = self.cluster.autoscale_tick(apply=autoscale_apply if autoscale_apply else None)
        rebalance_report = None
        if rebalance_auto:
            rebalance_report = self.cluster.rebalance_tick(
                force_apply=True if rebalance_apply else None
            )
        return {
            "health": report,
            "backups": backup_result,
            "failover": failover_actions,
            "drift": drift_reports,
            "recovery": recovery_report,
            "autoscale": autoscale_report,
            "rebalance_auto": rebalance_report,
        }

    def run(
        self,
        interval_sec: float,
        *,
        iterations: Optional[int] = None,
        backup: bool = False,
        incremental: bool = False,
        failover: bool = False,
        drift: bool = False,
        probe_sql: Optional[str] = None,
        check_replicas: bool = False,
        recover: bool = False,
        autoscale: bool = False,
        autoscale_apply: bool = False,
        rebalance_auto: bool = False,
        rebalance_apply: bool = False,
        matview_tick: bool = False,
        matview_force: bool = False,
        on_tick: Optional[Callable[[Dict[str, Any]], None]] = None,
    ) -> None:
        count = 0
        while iterations is None or count < iterations:
            snapshot = self.tick(
                backup=backup,
                incremental=incremental,
                failover=failover,
                drift=drift,
                probe_sql=probe_sql,
                check_replicas=check_replicas,
                recover=recover,
                autoscale=autoscale,
                autoscale_apply=autoscale_apply,
                rebalance_auto=rebalance_auto,
                rebalance_apply=rebalance_apply,
                matview_tick=matview_tick,
                matview_force=matview_force,
            )
            if on_tick:
                on_tick(snapshot)
            count += 1
            if iterations is not None and count >= iterations:
                break
            time.sleep(interval_sec)


def shard_ring_map(shard: QuasarShard, sample_keys: List[str]) -> Dict[str, str]:
    return {key: shard.node_for_key(key).name for key in sample_keys}


def repair_replica_set(rep: QuasarReplica) -> List[Path]:
    """Re-sync every replica file from the master via checkpoint + file copy."""
    master = rep.set.master
    rep.client.checkpoint_sql(database=master)
    repaired: List[Path] = []
    src_wal = Path(str(master) + ".wal")
    for replica_path in rep.set.replicas:
        if replica_path.exists():
            replica_path.unlink()
        dst_wal = Path(str(replica_path) + ".wal")
        if dst_wal.exists():
            dst_wal.unlink()
        shutil.copy2(master, replica_path)
        if src_wal.exists():
            shutil.copy2(src_wal, dst_wal)
        repaired.append(replica_path)
    return repaired


class QuasarUpgradeController:
    """Rolling zero-downtime upgrade state machine for shard fleets."""

    STAGES = ("idle", "preflight", "canary", "progressive", "verify", "completed", "rolled_back")

    def __init__(self, cluster: QuasarCluster, config: Optional[Dict[str, Any]] = None) -> None:
        self.cluster = cluster
        self.config = config or {}
        self._canary_shards = max(1, int(self.config.get("canary_shards", 1)))
        self._max_unhealthy_shards = max(0, int(self.config.get("max_unhealthy_shards", 0)))
        self._max_error_rate = float(self.config.get("max_error_rate", 0.20))
        self._state_file = Path(self.config.get("state_file", ".quasar/upgrades/state.json"))
        self._state_file.parent.mkdir(parents=True, exist_ok=True)
        self.state: Dict[str, Any] = {
            "stage": "idle",
            "target_version": None,
            "applied_shards": [],
            "errors": [],
        }
        self._load_state()

    def _load_state(self) -> None:
        if not self._state_file.exists():
            return
        try:
            import json

            self.state.update(json.loads(self._state_file.read_text(encoding="utf-8")))
        except Exception:
            return

    def _persist_state(self) -> None:
        import json

        self._state_file.write_text(json.dumps(self.state, indent=2), encoding="utf-8")

    def _violates_gate(self) -> Optional[str]:
        health = self.cluster.health()
        if not health.get("healthy"):
            return "cluster unhealthy"
        shards = health.get("shards", {}) or {}
        unhealthy = sum(1 for _, ok in shards.items() if not ok)
        if unhealthy > self._max_unhealthy_shards:
            return f"unhealthy shards {unhealthy} > allowed {self._max_unhealthy_shards}"
        mon = self.cluster.monitor.snapshot()
        if float(mon.get("error_rate", 0.0)) > self._max_error_rate:
            return "error rate gate violated"
        return None

    def start(self, target_version: str) -> Dict[str, Any]:
        self.state = {
            "stage": "preflight",
            "target_version": str(target_version),
            "applied_shards": [],
            "errors": [],
            "started_at": time.time(),
        }
        self._persist_state()
        return dict(self.state)

    def run_step(self) -> Dict[str, Any]:
        stage = self.state.get("stage", "idle")
        if stage == "idle":
            return dict(self.state)
        if stage == "preflight":
            err = self._violates_gate()
            if err:
                self.state["errors"].append(f"preflight failed: {err}")
                self.state["stage"] = "rolled_back"
                self._persist_state()
                return dict(self.state)
            self.state["stage"] = "canary"
            self._persist_state()
            return dict(self.state)
        if stage == "canary":
            pending = [n.name for n in self.cluster.shard.nodes if n.name not in self.state["applied_shards"]]
            for node_name in pending[: self._canary_shards]:
                self.state["applied_shards"].append(node_name)
            err = self._violates_gate()
            if err:
                self.state["errors"].append(f"canary failed: {err}")
                self.state["stage"] = "rolled_back"
                self._persist_state()
                return dict(self.state)
            self.state["stage"] = "progressive"
            self._persist_state()
            return dict(self.state)
        if stage == "progressive":
            remaining = [n.name for n in self.cluster.shard.nodes if n.name not in self.state["applied_shards"]]
            if remaining:
                self.state["applied_shards"].append(remaining[0])
                err = self._violates_gate()
                if err:
                    self.state["errors"].append(f"progressive failed: {err}")
                    self.state["stage"] = "rolled_back"
                self._persist_state()
                return dict(self.state)
            self.state["stage"] = "verify"
            self._persist_state()
            return dict(self.state)
        if stage == "verify":
            err = self._violates_gate()
            if err:
                self.state["errors"].append(f"post-upgrade verification failed: {err}")
                self.state["stage"] = "rolled_back"
            else:
                self.state["stage"] = "completed"
            self._persist_state()
            return dict(self.state)
        return dict(self.state)
