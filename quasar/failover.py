"""Automatic shard failover with persistent routing state and anti-flap controls."""

from __future__ import annotations

import json
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from quasar.client import AstralDBClient
from quasar.quasar import QuasarShard, ShardNode

PathLike = str | Path


@dataclass(frozen=True)
class FailoverPolicy:
    failure_threshold: int = 3
    cooldown_after_promote_sec: float = 30.0
    fail_back_to_primary: bool = True
    health_probe_sql: str = "SELECT 1;"

    @classmethod
    def from_config(cls, raw: Dict[str, Any]) -> "FailoverPolicy":
        return cls(
            failure_threshold=max(1, int(raw.get("failure_threshold", 3))),
            cooldown_after_promote_sec=float(raw.get("cooldown_after_promote_sec", 30.0)),
            fail_back_to_primary=bool(raw.get("fail_back_to_primary", True)),
            health_probe_sql=str(raw.get("health_probe_sql", "SELECT 1;")),
        )


@dataclass
class FailoverTarget:
    """Primary + ordered standbys for one logical shard."""

    shard_name: str
    primary: Path
    standbys: List[Path] = field(default_factory=list)

    def all_paths(self) -> List[Path]:
        return [self.primary, *self.standbys]


class QuasarFailover:
    """
    Track active database path per shard; promote standbys when primary is unhealthy.

    Uses consecutive failure counts before promotion and optional fail-back when the
    primary recovers. State: active paths, health streaks, promotion timestamps.
    """

    def __init__(
        self,
        shard: QuasarShard,
        targets: Dict[str, FailoverTarget],
        client: Optional[AstralDBClient] = None,
        *,
        state_file: PathLike,
        auto_promote: bool = True,
        policy: Optional[FailoverPolicy] = None,
    ) -> None:
        self.shard = shard
        self.targets = targets
        self.client = client or shard.client
        self.state_file = Path(state_file)
        self.auto_promote = auto_promote
        self.policy = policy or FailoverPolicy()
        self._state = self._load_state()

    def _load_state(self) -> Dict[str, Any]:
        if not self.state_file.is_file():
            return self._empty_state()
        try:
            data = json.loads(self.state_file.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return self._empty_state()
        if not isinstance(data, dict):
            return self._empty_state()
        data.setdefault("active", {})
        data.setdefault("history", [])
        data.setdefault("health_streak", {})
        data.setdefault("promoted_at", {})
        data.setdefault("using_standby", {})
        return data

    @staticmethod
    def _empty_state() -> Dict[str, Any]:
        return {
            "active": {},
            "history": [],
            "health_streak": {},
            "promoted_at": {},
            "using_standby": {},
            "version": 2,
        }

    def _save_state(self) -> None:
        from quasar.security import atomic_write_json

        atomic_write_json(self.state_file, self._state)

    def _probe(self, database: Path) -> bool:
        try:
            self.client.query(self.policy.health_probe_sql, database=database, immediate=True)
            return True
        except RuntimeError:
            return False

    def _record_health(self, shard_name: str, healthy: bool) -> int:
        streak = self._state.setdefault("health_streak", {})
        entry = streak.setdefault(shard_name, {"failures": 0, "successes": 0})
        if healthy:
            entry["failures"] = 0
            entry["successes"] = int(entry.get("successes", 0)) + 1
        else:
            entry["successes"] = 0
            entry["failures"] = int(entry.get("failures", 0)) + 1
        return int(entry["failures"])

    def _cooldown_elapsed(self, shard_name: str) -> bool:
        promoted_at = self._state.get("promoted_at", {}).get(shard_name)
        if promoted_at is None:
            return True
        return (time.time() - float(promoted_at)) >= self.policy.cooldown_after_promote_sec

    def active_database(self, node: ShardNode) -> Path:
        active = self._state.get("active", {})
        path = active.get(node.name)
        if path:
            return Path(path)
        return node.database

    def resolve_nodes(self) -> List[ShardNode]:
        return [
            ShardNode(name=n.name, database=self.active_database(n)) for n in self.shard.nodes
        ]

    def health_report(self) -> Dict[str, Any]:
        report: Dict[str, Any] = {
            "shards": {},
            "active": dict(self._state.get("active", {})),
            "health_streak": dict(self._state.get("health_streak", {})),
            "using_standby": dict(self._state.get("using_standby", {})),
            "policy": {
                "failure_threshold": self.policy.failure_threshold,
                "cooldown_after_promote_sec": self.policy.cooldown_after_promote_sec,
                "fail_back_to_primary": self.policy.fail_back_to_primary,
            },
        }
        for name, target in self.targets.items():
            shard_health: Dict[str, Any] = {
                "primary": self._probe(target.primary),
                "primary_path": str(target.primary),
            }
            standbys = []
            for i, standby in enumerate(target.standbys):
                standbys.append(
                    {
                        "index": i,
                        "path": str(standby),
                        "healthy": self._probe(standby),
                    }
                )
            shard_health["standbys"] = standbys
            shard_health["active"] = str(self.active_database(self._by_name(name)))
            shard_health["failures"] = self._state.get("health_streak", {}).get(name, {})
            report["shards"][name] = shard_health
        return report

    def _by_name(self, name: str) -> ShardNode:
        for n in self.shard.nodes:
            if n.name == name:
                return n
        raise KeyError(name)

    def promote(self, shard_name: str, *, standby_index: int = 0) -> Path:
        target = self.targets[shard_name]
        if standby_index >= len(target.standbys):
            raise IndexError(f"standby_index {standby_index} out of range for {shard_name}")
        standby = target.standbys[standby_index]
        if not self._probe(standby):
            raise RuntimeError(f"standby not healthy: {standby}")

        active_path = target.primary
        self.client.checkpoint_sql(database=standby)
        if active_path.exists():
            backup = active_path.with_suffix(active_path.suffix + ".failed")
            if backup.exists():
                backup.unlink()
            shutil.move(str(active_path), str(backup))
        shutil.copy2(standby, active_path)
        src_wal = Path(str(standby) + ".wal")
        dst_wal = Path(str(active_path) + ".wal")
        if src_wal.exists():
            if dst_wal.exists():
                dst_wal.unlink()
            shutil.copy2(src_wal, dst_wal)

        self._state.setdefault("active", {})[shard_name] = str(active_path.resolve())
        self._state.setdefault("using_standby", {})[shard_name] = True
        self._state.setdefault("promoted_at", {})[shard_name] = time.time()
        self._state.setdefault("health_streak", {}).setdefault(shard_name, {})["failures"] = 0
        self._state.setdefault("history", []).append(
            {
                "ts": time.time(),
                "shard": shard_name,
                "action": "promote",
                "standby": str(standby),
                "active": str(active_path),
            }
        )
        self._save_state()
        return active_path

    def fail_back(self, shard_name: str) -> Optional[Path]:
        """Route back to config primary when it is healthy again."""
        if not self.policy.fail_back_to_primary:
            return None
        if not self._state.get("using_standby", {}).get(shard_name):
            return None
        target = self.targets[shard_name]
        if not self._probe(target.primary):
            return None
        self._state.setdefault("active", {})[shard_name] = str(target.primary.resolve())
        self._state.setdefault("using_standby", {})[shard_name] = False
        self._state.setdefault("history", []).append(
            {
                "ts": time.time(),
                "shard": shard_name,
                "action": "fail_back",
                "active": str(target.primary),
            }
        )
        self._save_state()
        return target.primary

    def tick(self) -> List[Dict[str, Any]]:
        actions: List[Dict[str, Any]] = []
        if not self.auto_promote:
            for name, target in self.targets.items():
                if self._probe(target.primary):
                    self._state.setdefault("active", {})[name] = str(target.primary.resolve())
            self._save_state()
            return actions

        for name, target in self.targets.items():
            primary_ok = self._probe(target.primary)
            failures = self._record_health(name, primary_ok)

            if primary_ok:
                self._state.setdefault("active", {})[name] = str(target.primary.resolve())
                fb = self.fail_back(name)
                if fb is not None:
                    actions.append({"shard": name, "fail_back": str(fb)})
                continue

            if failures < self.policy.failure_threshold:
                actions.append({"shard": name, "status": "degraded", "failures": failures})
                continue

            if not self._cooldown_elapsed(name):
                actions.append({"shard": name, "status": "cooldown"})
                continue

            promoted = False
            for i, standby in enumerate(target.standbys):
                if self._probe(standby):
                    path = self.promote(name, standby_index=i)
                    actions.append({"shard": name, "promoted": str(path), "standby_index": i})
                    promoted = True
                    break
            if not promoted:
                actions.append({"shard": name, "error": "no healthy standby"})

        self._save_state()
        return actions

    def reset(self) -> None:
        self._state = self._empty_state()
        self._save_state()
