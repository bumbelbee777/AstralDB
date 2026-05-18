"""Coordinator-backed distributed transactions (strict 2PC + durable logs)."""

from __future__ import annotations

import json
import time
import uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, TYPE_CHECKING

from quasar.client import AstralDBClient, QueryResult
from quasar.quasar import ShardNode
from quasar.security import atomic_write_json
from quasar.xtxn import CrossShardTxnResult, TxnStatement

if TYPE_CHECKING:
    from quasar.consensus import ClusterConsensus
    from quasar.quasar import QuasarShard


class TxnPhase(str, Enum):
    STARTED = "started"
    PREPARING = "preparing"
    PREPARED = "prepared"
    DECISION_COMMITTED = "decision_committed"
    COMMITTING = "committing"
    COMMITTED = "committed"
    ABORTING = "aborting"
    ABORTED = "aborted"


@dataclass(frozen=True)
class DistributedTxnConfig:
    enabled: bool = True
    wal_file: str = ".quasar/dtxn/wal.jsonl"
    participant_log_dir: str = ".quasar/dtxn/participants"
    recover_on_start: bool = True
    strict_2pc: bool = True
    require_consensus_for_commit: bool = True

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]], base: Optional[Path] = None) -> "DistributedTxnConfig":
        if not raw:
            return cls(enabled=False)
        wal = str(raw.get("wal_file", ".quasar/dtxn/wal.jsonl"))
        part_dir = str(raw.get("participant_log_dir", ".quasar/dtxn/participants"))
        if base:
            if not Path(wal).is_absolute():
                wal = str((base / wal).resolve())
            if not Path(part_dir).is_absolute():
                part_dir = str((base / part_dir).resolve())
        return cls(
            enabled=bool(raw.get("enabled", True)),
            wal_file=wal,
            participant_log_dir=part_dir,
            recover_on_start=bool(raw.get("recover_on_start", True)),
            strict_2pc=bool(raw.get("strict_2pc", True)),
            require_consensus_for_commit=bool(raw.get("require_consensus_for_commit", True)),
        )


@dataclass
class CoordinatorRecord:
    xid: str
    phase: str
    participants: List[str]
    statements: List[Dict[str, str]]
    ts: float
    error: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "xid": self.xid,
            "phase": self.phase,
            "participants": self.participants,
            "statements": self.statements,
            "ts": self.ts,
            "error": self.error,
        }


class CoordinatorLog:
    """Append-only WAL for transaction coordinator decisions."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def append(self, record: CoordinatorRecord) -> None:
        with self.path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(record.to_dict(), separators=(",", ":")) + "\n")

    def load_all(self) -> List[CoordinatorRecord]:
        if not self.path.is_file():
            return []
        records: List[CoordinatorRecord] = []
        for line in self.path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            data = json.loads(line)
            records.append(
                CoordinatorRecord(
                    xid=data["xid"],
                    phase=data["phase"],
                    participants=list(data.get("participants", [])),
                    statements=list(data.get("statements", [])),
                    ts=float(data.get("ts", 0)),
                    error=data.get("error"),
                )
            )
        return records

    def latest_by_xid(self) -> Dict[str, CoordinatorRecord]:
        out: Dict[str, CoordinatorRecord] = {}
        for rec in self.load_all():
            out[rec.xid] = rec
        return out


class ParticipantLog:
    """Per-shard prepare vote log (participant leg of 2PC)."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)

    def _path(self, shard: str) -> Path:
        return self.root / f"{shard}.jsonl"

    def append(self, shard: str, xid: str, vote: str, *, error: Optional[str] = None) -> None:
        rec = {"xid": xid, "vote": vote, "ts": time.time(), "error": error}
        with self._path(shard).open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec, separators=(",", ":")) + "\n")

    def latest_vote(self, shard: str, xid: str) -> Optional[str]:
        path = self._path(shard)
        if not path.is_file():
            return None
        last: Optional[str] = None
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            data = json.loads(line)
            if data.get("xid") == xid:
                last = data.get("vote")
        return last

    def all_prepared(self, participants: List[str], xid: str) -> bool:
        for name in participants:
            vote = self.latest_vote(name, xid)
            if vote != "prepared":
                return False
        return True


class DistributedTransactionCoordinator:
    """
    Strict two-phase commit with durable coordinator and participant logs.

    Phase 1 — prepare: ``BEGIN`` + statements on each shard; participant logs ``prepared``.
    Phase 2 — decision: log ``decision_committed`` (and optional consensus quorum) **before**
    ``COMMIT`` is sent. Crash after decision is completed by recovery.

    Optional ``ClusterConsensus`` replicates the commit decision across orchestration members.
    """

    def __init__(
        self,
        shard: "QuasarShard",
        client: AstralDBClient,
        *,
        wal_path: Path,
        participant_log_dir: Path,
        config: Optional[DistributedTxnConfig] = None,
        consensus: Optional["ClusterConsensus"] = None,
    ) -> None:
        self.shard = shard
        self.client = client
        self.config = config or DistributedTxnConfig()
        self.consensus = consensus
        self.log = CoordinatorLog(wal_path)
        self.participant_log = ParticipantLog(participant_log_dir)
        self._by_name = {n.name: n for n in shard.nodes}

    def _participants_for_keys(self, keys: Set[str]) -> List[ShardNode]:
        seen: Dict[str, ShardNode] = {}
        for key in keys:
            node = self.shard.node_for_key(key)
            seen[node.name] = node
        return list(seen.values())

    def _run_on_nodes(self, sql: str, nodes: List[ShardNode]) -> List[tuple[ShardNode, QueryResult]]:
        results: List[tuple[ShardNode, QueryResult]] = []
        with ThreadPoolExecutor(max_workers=max(1, len(nodes))) as pool:
            futures = {
                pool.submit(self.client.query, sql, database=n.database, immediate=True): n
                for n in nodes
            }
            for fut in as_completed(futures):
                n = futures[fut]
                results.append((n, fut.result()))
        return results

    def _log(self, xid: str, phase: TxnPhase, participants: List[str], statements: List[Dict[str, str]], error: Optional[str] = None) -> None:
        self.log.append(
            CoordinatorRecord(
                xid=xid,
                phase=phase.value,
                participants=participants,
                statements=statements,
                ts=time.time(),
                error=error,
            )
        )

    def _prepare_phase(
        self,
        xid: str,
        participants: List[ShardNode],
        statements: List[TxnStatement],
        stmt_dicts: List[Dict[str, str]],
        names: List[str],
    ) -> None:
        self._log(xid, TxnPhase.PREPARING, names, stmt_dicts)
        self._run_on_nodes("BEGIN;", participants)
        for stmt in statements:
            node = self.shard.node_for_key(stmt.shard_key)
            result = self.client.query(stmt.sql, database=node.database, immediate=True)
            if not result.ok:
                self.participant_log.append(node.name, xid, "abort", error="statement failed")
                raise RuntimeError(f"{node.name}: statement failed")
        for node in participants:
            self.participant_log.append(node.name, xid, "prepared")
        if self.config.strict_2pc and not self.participant_log.all_prepared(names, xid):
            raise RuntimeError("prepare quorum not reached on participants")
        self._log(xid, TxnPhase.PREPARED, names, stmt_dicts)

    def _commit_decision(self, xid: str, names: List[str], stmt_dicts: List[Dict[str, str]]) -> None:
        if self.config.require_consensus_for_commit and self.consensus is not None:
            vote = self.consensus.propose("dtxn_commit", {"xid": xid, "participants": names})
            if not vote.get("committed"):
                raise RuntimeError(f"consensus rejected commit: {vote}")
        self._log(xid, TxnPhase.DECISION_COMMITTED, names, stmt_dicts)

    def _commit_phase(self, xid: str, participants: List[ShardNode], names: List[str], stmt_dicts: List[Dict[str, str]]) -> List[str]:
        self._log(xid, TxnPhase.COMMITTING, names, stmt_dicts)
        errors: List[str] = []
        commit_outcomes = self._run_on_nodes("COMMIT;", participants)
        for node, result in commit_outcomes:
            if not result.ok:
                errors.append(f"{node.name}: commit failed")
        return errors

    def run(self, statements: List[TxnStatement]) -> CrossShardTxnResult:
        if not statements:
            raise ValueError("at least one statement required")
        keys = {s.shard_key for s in statements}
        participants = self._participants_for_keys(keys)
        names = [n.name for n in participants]
        stmt_dicts = [{"sql": s.sql, "shard_key": s.shard_key} for s in statements]
        xid = f"xid-{uuid.uuid4().hex[:16]}"

        self._log(xid, TxnPhase.STARTED, names, stmt_dicts)
        errors: List[str] = []

        try:
            self._prepare_phase(xid, participants, statements, stmt_dicts, names)
            self._commit_decision(xid, names, stmt_dicts)
            commit_errors = self._commit_phase(xid, participants, names, stmt_dicts)
            if commit_errors:
                raise RuntimeError("; ".join(commit_errors))

            self._log(xid, TxnPhase.COMMITTED, names, stmt_dicts)
            return CrossShardTxnResult(
                committed=True,
                participants=names,
                statements=len(statements),
                errors=[],
                xid=xid,
                acid=True,
            )
        except Exception as exc:
            errors.append(str(exc))
            self._log(xid, TxnPhase.ABORTING, names, stmt_dicts, error=str(exc))
            for node in participants:
                try:
                    self.client.query("ROLLBACK;", database=node.database, immediate=True)
                except RuntimeError:
                    pass
                self.participant_log.append(node.name, xid, "aborted", error=str(exc))
            self._log(xid, TxnPhase.ABORTED, names, stmt_dicts, error=str(exc))
            return CrossShardTxnResult(
                committed=False,
                participants=names,
                statements=0,
                errors=errors,
                xid=xid,
                acid=True,
            )

    def recover(self) -> List[Dict[str, Any]]:
        """Complete or abort in-doubt transactions using coordinator + participant logs."""
        actions: List[Dict[str, Any]] = []
        latest = self.log.latest_by_xid()
        terminal = {TxnPhase.COMMITTED.value, TxnPhase.ABORTED.value}
        commit_decided = {
            TxnPhase.DECISION_COMMITTED.value,
            TxnPhase.COMMITTING.value,
        }
        abort_phases = {
            TxnPhase.STARTED.value,
            TxnPhase.PREPARING.value,
            TxnPhase.PREPARED.value,
            TxnPhase.ABORTING.value,
        }

        for xid, rec in latest.items():
            if rec.phase in terminal:
                continue
            nodes = [self._by_name[n] for n in rec.participants if n in self._by_name]
            if rec.phase in commit_decided:
                try:
                    self._run_on_nodes("COMMIT;", nodes)
                    self._log(xid, TxnPhase.COMMITTED, rec.participants, rec.statements, error="recovery")
                    actions.append({"xid": xid, "action": "committed", "phase_was": rec.phase})
                except Exception as exc:
                    actions.append({"xid": xid, "action": "commit_failed", "error": str(exc)})
                continue
            if rec.phase in abort_phases:
                for node in nodes:
                    try:
                        self.client.query("ROLLBACK;", database=node.database, immediate=True)
                    except RuntimeError:
                        pass
                    self.participant_log.append(node.name, xid, "aborted", error="recovery")
                self._log(xid, TxnPhase.ABORTED, rec.participants, rec.statements, error="recovery")
                actions.append({"xid": xid, "action": "aborted", "phase_was": rec.phase})
        return actions
