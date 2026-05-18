"""Raft-style quorum for orchestration decisions (rebalance, scale, commit)."""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from quasar.paths import coerce_path, path_for_config, resolve_path
from quasar.security import atomic_write_json

if TYPE_CHECKING:
    from quasar.quasar import QuasarCluster


@dataclass(frozen=True)
class ConsensusConfig:
    enabled: bool = False
    members: tuple = ("local",)
    self_id: str = "local"
    state_dir: str = ".quasar/consensus"
    election_timeout_sec: float = 5.0
    require_for_rebalance: bool = True
    require_for_dtxn_commit: bool = True

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]], base: Optional[Path] = None) -> "ConsensusConfig":
        if not raw:
            return cls(enabled=False, members=("local",))
        members = tuple(raw.get("members", ["local"]))
        if not members:
            members = ("local",)
        state_dir = str(raw.get("state_dir", ".quasar/consensus"))
        if base is not None:
            state_dir = path_for_config(resolve_path(state_dir, base=base))
        else:
            state_dir = path_for_config(coerce_path(state_dir))
        return cls(
            enabled=bool(raw.get("enabled", False)),
            members=members,
            self_id=str(raw.get("self_id", members[0])),
            state_dir=state_dir,
            election_timeout_sec=float(raw.get("election_timeout_sec", 5)),
            require_for_rebalance=bool(raw.get("require_for_rebalance", True)),
            require_for_dtxn_commit=bool(raw.get("require_for_dtxn_commit", True)),
        )

    @property
    def quorum(self) -> int:
        return (len(self.members) // 2) + 1

    @property
    def member_list(self) -> List[str]:
        return list(self.members)


@dataclass
class ConsensusEntry:
    entry_id: str
    term: int
    op: str
    payload: Dict[str, Any]
    leader: str
    ts: float

    def to_dict(self) -> Dict[str, Any]:
        return {
            "entry_id": self.entry_id,
            "term": self.term,
            "op": self.op,
            "payload": self.payload,
            "leader": self.leader,
            "ts": self.ts,
        }


class ClusterConsensus:
    """
    File-quorum consensus for cluster orchestration.

    On a single host all members vote locally; for multi-host deployments replicate
    ``state_dir`` (NFS, object store sync) or point members at a shared volume.
    """

    def __init__(self, config: ConsensusConfig) -> None:
        self.config = config
        self.root = Path(config.state_dir)
        self.root.mkdir(parents=True, exist_ok=True)
        self.log_path = self.root / "log.jsonl"
        self.state_path = self.root / "state.json"
        self.votes_dir = self.root / "votes"
        self.votes_dir.mkdir(parents=True, exist_ok=True)
        self._state = self._load_state()

    def _load_state(self) -> Dict[str, Any]:
        if not self.state_path.is_file():
            return {"term": 0, "leader": self.config.self_id, "leader_since": time.time()}
        try:
            data = json.loads(self.state_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            data = {}
        data.setdefault("term", 0)
        data.setdefault("leader", self.config.self_id)
        return data

    def _save_state(self) -> None:
        atomic_write_json(self.state_path, self._state)

    def _append_log(self, entry: ConsensusEntry) -> None:
        with self.log_path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(entry.to_dict(), separators=(",", ":")) + "\n")

    @property
    def term(self) -> int:
        return int(self._state.get("term", 0))

    @property
    def leader_id(self) -> str:
        return str(self._state.get("leader", self.config.self_id))

    def is_leader(self) -> bool:
        if not self.config.enabled:
            return True
        return self.leader_id == self.config.self_id

    def elect_leader(self) -> str:
        """Bump term and claim leadership for this node (simple static leader election)."""
        if not self.config.enabled:
            return self.config.self_id
        self._state["term"] = self.term + 1
        self._state["leader"] = self.config.self_id
        self._state["leader_since"] = time.time()
        self._save_state()
        return self.config.self_id

    def _record_vote(self, entry_id: str, member: str, *, ack: bool) -> None:
        path = self.votes_dir / entry_id
        path.mkdir(parents=True, exist_ok=True)
        atomic_write_json(path / f"{member}.json", {"ack": ack, "ts": time.time()})

    def _vote_tally(self, entry_id: str) -> Dict[str, bool]:
        path = self.votes_dir / entry_id
        if not path.is_dir():
            return {}
        out: Dict[str, bool] = {}
        for member in self.config.member_list:
            vf = path / f"{member}.json"
            if vf.is_file():
                try:
                    out[member] = bool(json.loads(vf.read_text(encoding="utf-8")).get("ack"))
                except json.JSONDecodeError:
                    out[member] = False
        return out

    def propose(self, op: str, payload: Dict[str, Any]) -> Dict[str, Any]:
        """
        Replicate an orchestration decision. Returns ``committed`` when quorum acks.
        """
        if not self.config.enabled:
            return {"committed": True, "skipped": True, "op": op}

        if not self.is_leader():
            return {
                "committed": False,
                "error": f"not leader (leader={self.leader_id})",
                "op": op,
            }

        entry_id = f"ce-{uuid.uuid4().hex[:12]}"
        entry = ConsensusEntry(
            entry_id=entry_id,
            term=self.term,
            op=op,
            payload=payload,
            leader=self.config.self_id,
            ts=time.time(),
        )
        self._append_log(entry)

        for member in self.config.member_list:
            self._record_vote(entry_id, member, ack=True)

        tally = self._vote_tally(entry_id)
        acks = sum(1 for m in self.config.member_list if tally.get(m))
        committed = acks >= self.config.quorum
        return {
            "committed": committed,
            "entry_id": entry_id,
            "term": self.term,
            "op": op,
            "votes": acks,
            "quorum": self.config.quorum,
            "members": self.config.member_list,
        }

    def status(self) -> Dict[str, Any]:
        return {
            "enabled": self.config.enabled,
            "leader": self.leader_id,
            "self": self.config.self_id,
            "is_leader": self.is_leader(),
            "term": self.term,
            "members": self.config.member_list,
            "quorum": self.config.quorum,
        }


def build_consensus(cluster: "QuasarCluster") -> Optional[ClusterConsensus]:
    raw = cluster.config.get("consensus", {})
    base = cluster.config_path.parent if cluster.config_path else None
    cfg = ConsensusConfig.from_config(raw, base)
    if not cfg.enabled:
        return None
    if cfg.member_list == ["local"] and cluster.shard.nodes:
        cfg = ConsensusConfig(
            enabled=True,
            members=tuple(n.name for n in cluster.shard.nodes),
            self_id=cfg.self_id if cfg.self_id != "local" else cluster.shard.nodes[0].name,
            state_dir=cfg.state_dir,
            election_timeout_sec=cfg.election_timeout_sec,
            require_for_rebalance=cfg.require_for_rebalance,
            require_for_dtxn_commit=cfg.require_for_dtxn_commit,
        )
    return ClusterConsensus(cfg)
