"""Automatic shard failover with persistent routing state."""

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

    State is persisted as JSON: {"active": {"shard0": "/path/db"}, "history": [...]}
    """

    def __init__(
        self,
        shard: QuasarShard,
        targets: Dict[str, FailoverTarget],
        client: Optional[AstralDBClient] = None,
        *,
        state_file: PathLike,
        auto_promote: bool = True,
    ) -> None:
        self.shard = shard
        self.targets = targets
        self.client = client or shard.client
        self.state_file = Path(state_file)
        self.auto_promote = auto_promote
        self._state = self._load_state()

    def _load_state(self) -> Dict[str, Any]:
        if not self.state_file.is_file():
            return {"active": {}, "history": [], "version": 1}
        try:
            data = json.loads(self.state_file.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {"active": {}, "history": [], "version": 1}
        if not isinstance(data, dict):
            return {"active": {}, "history": [], "version": 1}
        data.setdefault("active", {})
        data.setdefault("history", [])
        return data

    def _save_state(self) -> None:
        from quasar.security import atomic_write_json

        atomic_write_json(self.state_file, self._state)

    def active_database(self, node: ShardNode) -> Path:
        """Resolved DB path for writes/reads (may differ from config after failover)."""
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
        report: Dict[str, Any] = {"shards": {}, "active": dict(self._state.get("active", {}))}
        for name, target in self.targets.items():
            shard_health: Dict[str, Any] = {"primary": self.client.health(database=target.primary)}
            standbys = []
            for i, standby in enumerate(target.standbys):
                standbys.append(
                    {"index": i, "path": str(standby), "healthy": self.client.health(database=standby)}
                )
            shard_health["standbys"] = standbys
            shard_health["active"] = str(self.active_database(self._by_name(name)))
            report["shards"][name] = shard_health
        return report

    def _by_name(self, name: str) -> ShardNode:
        for n in self.shard.nodes:
            if n.name == name:
                return n
        raise KeyError(name)

    def promote(self, shard_name: str, *, standby_index: int = 0) -> Path:
        """Promote a standby to active; optionally recopy to primary path."""
        target = self.targets[shard_name]
        if standby_index >= len(target.standbys):
            raise IndexError(f"standby_index {standby_index} out of range for {shard_name}")
        standby = target.standbys[standby_index]
        if not self.client.health(database=standby):
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

    def tick(self) -> List[Dict[str, Any]]:
        """One failover pass; returns actions taken."""
        actions: List[Dict[str, Any]] = []
        if not self.auto_promote:
            return actions
        for name, target in self.targets.items():
            primary_ok = self.client.health(database=target.primary)
            if primary_ok:
                self._state.setdefault("active", {})[name] = str(target.primary.resolve())
                continue
            for i, standby in enumerate(target.standbys):
                if self.client.health(database=standby):
                    path = self.promote(name, standby_index=i)
                    actions.append({"shard": name, "promoted": str(path), "standby_index": i})
                    break
            else:
                actions.append({"shard": name, "error": "no healthy standby"})
        self._save_state()
        return actions

    def reset(self) -> None:
        """Reset routing to config primaries."""
        self._state = {"active": {}, "history": [], "version": 1}
        self._save_state()
