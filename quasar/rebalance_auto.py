"""Automatic shard rebalancing when the ring or cluster membership changes."""

from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from quasar.quasar import QuasarCluster


@dataclass(frozen=True)
class RebalanceAutomationConfig:
    enabled: bool = False
    auto_apply: bool = False
    interval_sec: float = 0.0
    delete_from_source: bool = False
    batch_size: int = 100
    max_moves_per_tick: int = 5000
    require_consensus: bool = True
    tables: Optional[List[str]] = None

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]]) -> "RebalanceAutomationConfig":
        if not raw:
            return cls(enabled=False)
        tables = raw.get("tables")
        return cls(
            enabled=bool(raw.get("enabled", False)),
            auto_apply=bool(raw.get("auto_apply", False)),
            interval_sec=float(raw.get("interval_sec", 0)),
            delete_from_source=bool(raw.get("delete_from_source", False)),
            batch_size=int(raw.get("batch_size", 100)),
            max_moves_per_tick=int(raw.get("max_moves_per_tick", 5000)),
            require_consensus=bool(raw.get("require_consensus", True)),
            tables=list(tables) if tables else None,
        )


class QuasarRebalanceAutomator:
    """Plan and optionally apply rebalance moves on a schedule or after scale-out."""

    def __init__(self, cluster: "QuasarCluster", config: RebalanceAutomationConfig) -> None:
        self.cluster = cluster
        self.config = config
        self._last_run: float = 0.0

    def current_shard_names(self) -> List[str]:
        return [n.name for n in self.cluster.shard.nodes]

    def tick(self, *, force_apply: Optional[bool] = None) -> Dict[str, Any]:
        """Evaluate ring vs data ownership; plan and optionally apply moves."""
        if not self.config.enabled:
            return {"skipped": True}

        names = self.current_shard_names()
        plan = self.cluster.rebalance.plan(names, tables=self.config.tables)
        report: Dict[str, Any] = {
            "ts": time.time(),
            "move_count": len(plan.moves),
            "summary": plan.summary,
        }

        if not plan.moves:
            report["action"] = "none"
            return report

        report["action"] = "plan"
        do_apply = self.config.auto_apply if force_apply is None else force_apply
        if not do_apply:
            report["plan"] = plan.to_dict()
            return report

        if self.config.require_consensus and self.cluster.consensus is not None:
            vote = self.cluster.consensus.propose(
                "rebalance_apply",
                {"move_count": len(plan.moves), "shards": names},
            )
            report["consensus"] = vote
            if not vote.get("committed"):
                report["action"] = "blocked"
                return report

        capped_moves = plan.moves[: self.config.max_moves_per_tick]
        if len(capped_moves) < len(plan.moves):
            plan.moves = capped_moves
            report["truncated"] = True

        apply_result = self.cluster.rebalance.apply(
            plan,
            delete_from_source=self.config.delete_from_source,
            batch_size=self.config.batch_size,
        )
        report["action"] = "applied"
        report["apply"] = apply_result
        self._last_run = time.time()

        journal = getattr(self.cluster, "journal", None)
        if journal:
            journal.record(
                "REBALANCE_AUTO",
                detail=f"moves={apply_result.get('applied', 0)}",
                outcome="OK",
            )
        return report
