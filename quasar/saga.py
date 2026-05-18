"""Saga orchestration with compensating transactions for cross-shard financial flows."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from quasar.security import atomic_write_json

if TYPE_CHECKING:
    from quasar.quasar import QuasarCluster


@dataclass
class SagaStep:
    name: str
    sql: str
    shard_key: str
    compensate_sql: str = ""

    @classmethod
    def from_dict(cls, raw: Dict[str, Any]) -> "SagaStep":
        return cls(
            name=str(raw["name"]),
            sql=str(raw["sql"]),
            shard_key=str(raw["shard_key"]),
            compensate_sql=str(raw.get("compensate_sql", "")),
        )


@dataclass
class SagaResult:
    saga_id: str
    status: str
    completed_steps: List[str] = field(default_factory=list)
    compensated_steps: List[str] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "saga_id": self.saga_id,
            "status": self.status,
            "completed_steps": self.completed_steps,
            "compensated_steps": self.compensated_steps,
            "errors": self.errors,
        }


class QuasarSaga:
    """
    Forward steps run in order; on failure, completed steps are compensated in reverse.

    State is persisted under ``saga_state_dir`` for crash recovery inspection (not auto-resume).
    """

    def __init__(
        self,
        cluster: "QuasarCluster",
        *,
        state_dir: Path,
        journal=None,
    ) -> None:
        self.cluster = cluster
        self.state_dir = state_dir
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.journal = journal

    def _state_path(self, saga_id: str) -> Path:
        safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in saga_id)[:128]
        return self.state_dir / f"{safe}.json"

    def _persist(self, saga_id: str, result: SagaResult) -> None:
        atomic_write_json(self._state_path(saga_id), result.to_dict())

    def run(self, saga_id: str, steps: List[SagaStep]) -> SagaResult:
        result = SagaResult(saga_id=saga_id, status="running")
        self._persist(saga_id, result)
        if self.journal:
            self.journal.record("SAGA_START", detail=saga_id, extra={"steps": len(steps)})

        for step in steps:
            try:
                self.cluster.execute(step.sql, shard_key=step.shard_key)
                result.completed_steps.append(step.name)
                self._persist(saga_id, result)
                if self.journal:
                    self.journal.record(
                        "SAGA_STEP_OK",
                        detail=f"{saga_id}:{step.name}",
                        shard=self.cluster.shard.node_for_key(step.shard_key).name,
                    )
            except Exception as exc:
                result.errors.append(f"{step.name}: {exc}")
                result.status = "compensating"
                self._persist(saga_id, result)
                self._compensate(steps, result)
                result.status = "compensated" if not result.errors else "failed"
                self._persist(saga_id, result)
                if self.journal:
                    self.journal.record("SAGA_COMPENSATED", detail=saga_id, outcome=result.status)
                return result

        result.status = "completed"
        self._persist(saga_id, result)
        if self.journal:
            self.journal.record("SAGA_COMPLETE", detail=saga_id)
        return result

    def _compensate(self, steps: List[SagaStep], result: SagaResult) -> None:
        completed = {s.name: s for s in steps if s.name in result.completed_steps}
        for name in reversed(result.completed_steps):
            step = completed.get(name)
            if not step or not step.compensate_sql.strip():
                result.errors.append(f"{name}: no compensate_sql")
                continue
            try:
                self.cluster.execute(step.compensate_sql, shard_key=step.shard_key)
                result.compensated_steps.append(name)
            except Exception as exc:
                result.errors.append(f"compensate {name}: {exc}")

    def run_spec(self, spec: Dict[str, Any]) -> SagaResult:
        saga_id = str(spec.get("saga_id", "saga"))
        steps = [SagaStep.from_dict(s) for s in spec.get("steps", [])]
        if not steps:
            raise ValueError("saga spec requires at least one step")
        return self.run(saga_id, steps)
