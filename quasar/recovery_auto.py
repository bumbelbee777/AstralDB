"""Automated crash recovery: dtxn WAL, orphan rollbacks, pool heal."""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from quasar.quasar import QuasarCluster


@dataclass(frozen=True)
class RecoveryAutomationConfig:
    enabled: bool = True
    on_start: bool = True
    rollback_orphan_txns: bool = True
    recover_dtxn: bool = True
    heal_pool: bool = True
    interval_sec: float = 0.0

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]]) -> "RecoveryAutomationConfig":
        if not raw:
            return cls(enabled=False)
        return cls(
            enabled=bool(raw.get("enabled", True)),
            on_start=bool(raw.get("on_start", True)),
            rollback_orphan_txns=bool(raw.get("rollback_orphan_txns", True)),
            recover_dtxn=bool(raw.get("recover_dtxn", True)),
            heal_pool=bool(raw.get("heal_pool", True)),
            interval_sec=float(raw.get("interval_sec", 0)),
        )


class CrashRecoveryManager:
    """Run coordinated recovery steps after process or node crashes."""

    def __init__(self, cluster: "QuasarCluster", config: RecoveryAutomationConfig) -> None:
        self.cluster = cluster
        self.config = config
        self._lock = threading.Lock()
        self._last_run: float = 0.0
        self._background: Optional[threading.Thread] = None
        self._stop = threading.Event()

    def run(self, *, force: bool = False) -> Dict[str, Any]:
        """Execute recovery pipeline; no-op if disabled unless ``force``."""
        if not self.config.enabled and not force:
            return {"skipped": True}
        with self._lock:
            report: Dict[str, Any] = {"ts": time.time(), "steps": []}
            if self.config.recover_dtxn and self.cluster.dtxn is not None:
                dtxn_actions = self.cluster.dtxn.recover()
                report["dtxn"] = dtxn_actions
                report["steps"].append("dtxn")
            if self.config.rollback_orphan_txns:
                rolled = self._rollback_all_shards()
                report["rollback"] = rolled
                report["steps"].append("rollback")
            if self.config.heal_pool and hasattr(self.cluster.client, "heal"):
                self.cluster.client.heal()
                report["steps"].append("pool_heal")
            self._last_run = time.time()
            report["ok"] = True
            journal = getattr(self.cluster, "journal", None)
            if journal:
                journal.record(
                    "CRASH_RECOVERY",
                    detail=",".join(report["steps"]),
                    outcome="OK",
                )
            return report

    def _rollback_all_shards(self) -> List[str]:
        from quasar.recovery import rollback_database

        active = self.cluster.active_shard()
        rolled: List[str] = []
        for node in active.nodes:
            try:
                rollback_database(self.cluster.client, node.database)
                rolled.append(node.name)
            except Exception:
                pass
        return rolled

    def start_background(self) -> None:
        """Start periodic recovery when ``interval_sec`` > 0."""
        if self.config.interval_sec <= 0 or not self.config.enabled:
            return
        if self._background and self._background.is_alive():
            return
        self._stop.clear()

        def _loop() -> None:
            while not self._stop.is_set():
                self.run()
                self._stop.wait(self.config.interval_sec)

        self._background = threading.Thread(target=_loop, name="quasar-recovery", daemon=True)
        self._background.start()

    def stop_background(self) -> None:
        self._stop.set()
        if self._background:
            self._background.join(timeout=2.0)
            self._background = None
