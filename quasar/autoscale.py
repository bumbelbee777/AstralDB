"""Shard autoscaling from pool, workload, monitor, and disk signals."""

from __future__ import annotations

import json
import re
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from quasar.inventory import QuasarInventory
from quasar.security import atomic_write_json

if TYPE_CHECKING:
    from quasar.quasar import QuasarCluster


@dataclass(frozen=True)
class AutoscaleConfig:
    enabled: bool = False
    min_shards: int = 1
    max_shards: int = 32
    scale_out_step: int = 1
    cooldown_sec: float = 300.0
    auto_apply: bool = False
    rebalance_on_scale: bool = False
    state_file: str = ".quasar/autoscale.json"
    scale_out_pool_rejects: int = 1
    scale_out_error_rate: float = 0.15
    scale_out_mean_latency_ms: float = 750.0
    scale_out_max_shard_bytes: int = 0
    scale_in_error_rate: float = 0.02
    scale_in_mean_latency_ms: float = 100.0
    scale_in_max_shard_bytes: int = 0

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]]) -> "AutoscaleConfig":
        if not raw:
            return cls(enabled=False)
        return cls(
            enabled=bool(raw.get("enabled", False)),
            min_shards=max(1, int(raw.get("min_shards", 1))),
            max_shards=max(1, int(raw.get("max_shards", 32))),
            scale_out_step=max(1, int(raw.get("scale_out_step", 1))),
            cooldown_sec=float(raw.get("cooldown_sec", 300)),
            auto_apply=bool(raw.get("auto_apply", False)),
            rebalance_on_scale=bool(raw.get("rebalance_on_scale", False)),
            state_file=str(raw.get("state_file", ".quasar/autoscale.json")),
            scale_out_pool_rejects=int(raw.get("scale_out_pool_rejects", 1)),
            scale_out_error_rate=float(raw.get("scale_out_error_rate", 0.15)),
            scale_out_mean_latency_ms=float(raw.get("scale_out_mean_latency_ms", 750)),
            scale_out_max_shard_bytes=int(raw.get("scale_out_max_shard_bytes", 0)),
            scale_in_error_rate=float(raw.get("scale_in_error_rate", 0.02)),
            scale_in_mean_latency_ms=float(raw.get("scale_in_mean_latency_ms", 100)),
            scale_in_max_shard_bytes=int(raw.get("scale_in_max_shard_bytes", 0)),
        )


@dataclass
class AutoscaleMetrics:
    shard_count: int
    pool_rejected: int
    error_rate: float
    mean_latency_ms: float
    max_shard_bytes: int
    circuits_open: int
    unhealthy_shards: int

    def to_dict(self) -> Dict[str, Any]:
        return {
            "shard_count": self.shard_count,
            "pool_rejected": self.pool_rejected,
            "error_rate": self.error_rate,
            "mean_latency_ms": self.mean_latency_ms,
            "max_shard_bytes": self.max_shard_bytes,
            "circuits_open": self.circuits_open,
            "unhealthy_shards": self.unhealthy_shards,
        }


@dataclass
class AutoscaleDecision:
    action: str  # none | scale_out | scale_in | blocked
    reasons: List[str] = field(default_factory=list)
    metrics: Optional[AutoscaleMetrics] = None
    cooldown_remaining_sec: float = 0.0
    target_shard_count: Optional[int] = None

    def to_dict(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {
            "action": self.action,
            "reasons": self.reasons,
            "cooldown_remaining_sec": self.cooldown_remaining_sec,
        }
        if self.metrics:
            out["metrics"] = self.metrics.to_dict()
        if self.target_shard_count is not None:
            out["target_shard_count"] = self.target_shard_count
        return out


class QuasarAutoscale:
    """
    Recommend or apply horizontal shard scaling from orchestration metrics.

  Scale-out adds empty shard databases and updates ``cluster.json``.
  Scale-in only recommends draining a shard (no automatic data deletion).
    """

    def __init__(self, cluster: "QuasarCluster", config: AutoscaleConfig) -> None:
        self.cluster = cluster
        self.config = config
        base = cluster.config_path.parent if cluster.config_path else Path.cwd()
        self.state_file = base / config.state_file
        self._state = self._load_state()

    def _load_state(self) -> Dict[str, Any]:
        if not self.state_file.is_file():
            return {"history": [], "last_action_at": 0.0}
        try:
            data = json.loads(self.state_file.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {"history": [], "last_action_at": 0.0}
        if not isinstance(data, dict):
            return {"history": [], "last_action_at": 0.0}
        data.setdefault("history", [])
        data.setdefault("last_action_at", 0.0)
        return data

    def _save_state(self) -> None:
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        atomic_write_json(self.state_file, self._state)

    def _cooldown_remaining(self) -> float:
        elapsed = time.time() - float(self._state.get("last_action_at", 0))
        return max(0.0, self.config.cooldown_sec - elapsed)

    def collect_metrics(self) -> AutoscaleMetrics:
        pool = self.cluster.pool_stats()
        monitor = self.cluster.monitor.snapshot()
        workload = self.cluster.workload_stats()
        inv = QuasarInventory(self.cluster.shard.nodes).collect()
        health = self.cluster.health()

        circuits = workload.get("circuits", {})
        open_count = sum(1 for c in circuits.values() if isinstance(c, dict) and c.get("open"))

        shard_bytes = [int(s.get("bytes", 0)) for s in inv.get("shards", [])]
        unhealthy = sum(1 for ok in health.get("shards", {}).values() if not ok)

        return AutoscaleMetrics(
            shard_count=len(self.cluster.shard.nodes),
            pool_rejected=int(pool.get("rejected_queue_full", 0)),
            error_rate=float(monitor.get("error_rate", 0.0)),
            mean_latency_ms=float(monitor.get("mean_latency_ms", 0.0)),
            max_shard_bytes=max(shard_bytes) if shard_bytes else 0,
            circuits_open=open_count,
            unhealthy_shards=unhealthy,
        )

    def evaluate(self) -> AutoscaleDecision:
        if not self.config.enabled:
            return AutoscaleDecision(action="none", reasons=["autoscaling disabled"])

        metrics = self.collect_metrics()
        cooldown = self._cooldown_remaining()
        if cooldown > 0:
            return AutoscaleDecision(
                action="blocked",
                reasons=["cooldown"],
                metrics=metrics,
                cooldown_remaining_sec=cooldown,
            )

        reasons_out: List[str] = []
        if metrics.pool_rejected >= self.config.scale_out_pool_rejects:
            reasons_out.append("pool_rejected")
        if metrics.error_rate >= self.config.scale_out_error_rate:
            reasons_out.append("error_rate")
        if metrics.mean_latency_ms >= self.config.scale_out_mean_latency_ms:
            reasons_out.append("latency")
        if self.config.scale_out_max_shard_bytes > 0 and metrics.max_shard_bytes >= self.config.scale_out_max_shard_bytes:
            reasons_out.append("disk")
        if metrics.circuits_open > 0:
            reasons_out.append("circuits_open")
        if metrics.unhealthy_shards > 0:
            reasons_out.append("unhealthy_shards")

        if reasons_out and metrics.shard_count < self.config.max_shards:
            target = min(
                self.config.max_shards,
                metrics.shard_count + self.config.scale_out_step,
            )
            return AutoscaleDecision(
                action="scale_out",
                reasons=reasons_out,
                metrics=metrics,
                target_shard_count=target,
            )

        reasons_in: List[str] = []
        if metrics.shard_count > self.config.min_shards:
            if metrics.error_rate <= self.config.scale_in_error_rate:
                reasons_in.append("low_error_rate")
            if metrics.mean_latency_ms <= self.config.scale_in_mean_latency_ms:
                reasons_in.append("low_latency")
            if metrics.pool_rejected == 0 and metrics.circuits_open == 0:
                reasons_in.append("idle_pool")
            disk_ok = (
                self.config.scale_in_max_shard_bytes <= 0
                or metrics.max_shard_bytes <= self.config.scale_in_max_shard_bytes
            )
            if disk_ok and len(reasons_in) >= 3:
                return AutoscaleDecision(
                    action="scale_in",
                    reasons=reasons_in,
                    metrics=metrics,
                    target_shard_count=metrics.shard_count - 1,
                )

        return AutoscaleDecision(action="none", reasons=["within bounds"], metrics=metrics)

    def tick(self, *, apply: Optional[bool] = None) -> Dict[str, Any]:
        """Evaluate and optionally apply scaling (respects ``auto_apply`` when apply is None)."""
        decision = self.evaluate()
        report: Dict[str, Any] = {"decision": decision.to_dict(), "applied": None}
        do_apply = self.config.auto_apply if apply is None else apply
        if do_apply and decision.action == "scale_out":
            report["applied"] = self.apply_scale_out()
        elif decision.action == "scale_in":
            report["recommendation"] = self._scale_in_recommendation(decision)
        return report

    def _scale_in_recommendation(self, decision: AutoscaleDecision) -> Dict[str, Any]:
        """Scale-in is advisory only: drain and rebalance before removing a shard."""
        nodes = self.cluster.shard.nodes
        if len(nodes) <= self.config.min_shards:
            return {"skipped": True, "reason": "at min_shards"}
        victim = nodes[-1].name
        return {
            "drain_shard": victim,
            "steps": [
                f"Stop routing new keys to {victim} (update app or ring).",
                f"Run rebalance to move rows off {victim}.",
                f"Remove {victim} from cluster.json after data is empty.",
            ],
        }

    def apply_scale_out(self, *, count: Optional[int] = None) -> Dict[str, Any]:
        """Add shard(s), update cluster.json, warm databases, optional rebalance plan."""
        if not self.cluster.config_path:
            raise RuntimeError("cluster.config_path required to apply autoscale")
        if self._cooldown_remaining() > 0:
            return {"skipped": True, "reason": "cooldown", "remaining_sec": self._cooldown_remaining()}

        step = count if count is not None else self.config.scale_out_step
        added: List[Dict[str, str]] = []
        base = self.cluster.config_path.parent
        data_dir = base / "data"
        data_dir.mkdir(parents=True, exist_ok=True)

        raw = json.loads(self.cluster.config_path.read_text(encoding="utf-8"))
        shards = list(raw.get("shards", []))

        for _ in range(step):
            if len(shards) >= self.config.max_shards:
                break
            name = self._next_shard_name_from(shards)
            rel_db = f"data/{name}.db"
            shards.append({"name": name, "database": rel_db})
            added.append({"name": name, "database": rel_db})

        if not added:
            return {"skipped": True, "reason": "max_shards"}

        raw["shards"] = shards
        mm = raw.get("multi_master")
        if isinstance(mm, dict):
            for entry in added:
                mm[entry["name"]] = {"writers": [entry["database"]], "quorum": 1}

        atomic_write_json(self.cluster.config_path, raw)

        from quasar.config import load_cluster_config

        self.cluster.config = load_cluster_config(self.cluster.config_path)
        self.cluster.reload_shards()

        warmed: List[str] = []
        for entry in added:
            db = (base / entry["database"]).resolve()
            db.parent.mkdir(parents=True, exist_ok=True)
            try:
                self.cluster.client.query("SELECT 1;", database=db, immediate=True)
                warmed.append(entry["name"])
            except Exception:
                pass
            if hasattr(self.cluster.client, "warm"):
                self.cluster.client.warm([db])

        rebalance_hint = None
        if self.config.rebalance_on_scale:
            names = [s["name"] for s in shards]
            plan = self.cluster.rebalance.plan(names)
            rebalance_hint = plan.to_dict()

        self._state.setdefault("history", []).append(
            {"ts": time.time(), "action": "scale_out", "added": added}
        )
        self._state["last_action_at"] = time.time()
        self._save_state()

        journal = getattr(self.cluster, "journal", None)
        if journal:
            journal.record("AUTOSCALE_OUT", detail=",".join(a["name"] for a in added), outcome="OK")

        return {
            "added": added,
            "warmed": warmed,
            "shard_count": len(shards),
            "rebalance_plan": rebalance_hint,
            "note": "Run rebalance --apply to move data onto new shards when ready.",
        }

    def _next_shard_name_from(self, shards: List[Dict[str, Any]]) -> str:
        existing = {s["name"] for s in shards}
        numbers: List[int] = []
        for name in existing:
            m = re.match(r"shard(\d+)$", name)
            if m:
                numbers.append(int(m.group(1)))
        nxt = (max(numbers) + 1) if numbers else len(existing)
        candidate = f"shard{nxt}"
        while candidate in existing:
            nxt += 1
            candidate = f"shard{nxt}"
        return candidate
