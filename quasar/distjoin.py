"""Distributed cross-shard JOIN planning and execution helpers."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List

from quasar.crossjoin import CrossJoinSpec, QuasarCrossShardJoin


@dataclass
class DistributedJoinPlan:
    strategy: str
    estimated_left_rows: int
    estimated_right_rows: int
    shuffle_required: bool

    def to_dict(self) -> Dict[str, Any]:
        return {
            "strategy": self.strategy,
            "estimated_left_rows": self.estimated_left_rows,
            "estimated_right_rows": self.estimated_right_rows,
            "shuffle_required": self.shuffle_required,
        }


class DistributedJoinPlanner:
    """Simple cost-aware planner for cross-shard joins."""

    def __init__(self, row_broadcast_threshold: int = 5000) -> None:
        self.row_broadcast_threshold = row_broadcast_threshold

    def plan(self, datasets: Dict[str, List[Dict[str, Any]]], spec: CrossJoinSpec) -> DistributedJoinPlan:
        if not spec.joins:
            return DistributedJoinPlan("single_source", 0, 0, False)
        first = spec.joins[0]
        left_n = len(datasets.get(first.left, []))
        right_n = len(datasets.get(first.right, []))
        if min(left_n, right_n) <= self.row_broadcast_threshold:
            return DistributedJoinPlan("broadcast_hash_join", left_n, right_n, False)
        return DistributedJoinPlan("repartition_hash_join", left_n, right_n, True)


class QuasarDistributedJoinExecutor:
    def __init__(self, base: QuasarCrossShardJoin, planner: DistributedJoinPlanner) -> None:
        self.base = base
        self.planner = planner

    def execute(self, spec: CrossJoinSpec) -> Dict[str, Any]:
        # Reuse existing row fetch mechanics; overlay a planner + stream windowing surface.
        datasets: Dict[str, List[Dict[str, Any]]] = {}
        for t in spec.tables:
            datasets[t.alias] = self.base._fetch_table_rows(  # noqa: SLF001
                t,
                work_dir=self.base._work_dir(),  # noqa: SLF001
            )
        plan = self.planner.plan(datasets, spec)
        rows = self.base._join_datasets(datasets, spec)  # noqa: SLF001
        return {"plan": plan.to_dict(), "rows": rows, "count": len(rows)}

    def execute_stream(self, spec: CrossJoinSpec, *, chunk_size: int = 250) -> Dict[str, Any]:
        materialized = self.execute(spec)
        rows = materialized["rows"]
        chunks = [rows[i : i + max(1, chunk_size)] for i in range(0, len(rows), max(1, chunk_size))]
        return {"plan": materialized["plan"], "count": materialized["count"], "chunks": chunks}
