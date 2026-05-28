"""CDC commit envelopes, checkpoints, and sink publishing."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Protocol
from quasar.binary_ipc import encode_frames


@dataclass
class CdcEvent:
    commit_id: int
    timeline_id: str
    shard: str
    op: str
    table: str
    payload: Dict[str, Any]
    ts_unix: float

    def to_dict(self) -> Dict[str, Any]:
        return {
            "commit_id": self.commit_id,
            "timeline_id": self.timeline_id,
            "shard": self.shard,
            "op": self.op,
            "table": self.table,
            "payload": self.payload,
            "ts_unix": self.ts_unix,
        }


class CdcSink(Protocol):
    name: str

    def publish(self, events: List[CdcEvent]) -> None:
        ...


class FileCdcSink:
    def __init__(self, path: Path) -> None:
        self.name = "file"
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def publish(self, events: List[CdcEvent]) -> None:
        with self.path.open("a", encoding="utf-8") as fh:
            for ev in events:
                fh.write(json.dumps(ev.to_dict(), separators=(",", ":")) + "\n")


class BinaryFileCdcSink:
    def __init__(self, path: Path) -> None:
        self.name = "binary_file"
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def publish(self, events: List[CdcEvent]) -> None:
        payload = encode_frames([ev.to_dict() for ev in events])
        with self.path.open("ab") as fh:
            fh.write(payload)


class CdcCheckpointStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if not self.path.exists():
            self.path.write_text("{}", encoding="utf-8")

    def read(self) -> Dict[str, int]:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {}
        out: Dict[str, int] = {}
        if isinstance(data, dict):
            for k, v in data.items():
                try:
                    out[str(k)] = int(v)
                except (ValueError, TypeError):
                    continue
        return out

    def write(self, payload: Dict[str, int]) -> None:
        self.path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


class QuasarCdcPublisher:
    def __init__(
        self,
        *,
        timeline_id: str,
        checkpoints: CdcCheckpointStore,
        sinks: List[CdcSink],
        dlq_path: Optional[Path] = None,
    ) -> None:
        self.timeline_id = timeline_id
        self.checkpoints = checkpoints
        self.sinks = sinks
        self._events: List[CdcEvent] = []
        self._next_commit = 1
        self.dlq_path = dlq_path
        if self.dlq_path:
            self.dlq_path.parent.mkdir(parents=True, exist_ok=True)

    def record(self, *, shard: str, op: str, table: str, payload: Dict[str, Any]) -> CdcEvent:
        ev = CdcEvent(
            commit_id=self._next_commit,
            timeline_id=self.timeline_id,
            shard=shard,
            op=op,
            table=table,
            payload=payload,
            ts_unix=time.time(),
        )
        self._next_commit += 1
        self._events.append(ev)
        return ev

    def poll(self, *, after_commit: int = 0, limit: int = 1000) -> List[Dict[str, Any]]:
        return [e.to_dict() for e in self._events if e.commit_id > after_commit][:limit]

    def publish(self, consumer_group: str, *, limit: int = 500) -> Dict[str, Any]:
        offsets = self.checkpoints.read()
        after = offsets.get(consumer_group, 0)
        batch = [e for e in self._events if e.commit_id > after][:limit]
        if not batch:
            return {"published": 0, "checkpoint": after}
        published = 0
        for sink in self.sinks:
            try:
                sink.publish(batch)
                published += len(batch)
            except Exception as exc:
                if self.dlq_path:
                    with self.dlq_path.open("a", encoding="utf-8") as fh:
                        fh.write(
                            json.dumps(
                                {
                                    "sink": sink.name,
                                    "error": str(exc),
                                    "events": [b.to_dict() for b in batch],
                                }
                            )
                            + "\n"
                        )
                return {"published": 0, "checkpoint": after, "error": str(exc)}
        offsets[consumer_group] = batch[-1].commit_id
        self.checkpoints.write(offsets)
        return {"published": published, "checkpoint": offsets[consumer_group]}

    def publish_batched(
        self, consumer_group: str, *, limit: int = 500, chunk_size: int = 100
    ) -> Dict[str, Any]:
        offsets = self.checkpoints.read()
        after = offsets.get(consumer_group, 0)
        pending = [e for e in self._events if e.commit_id > after][:limit]
        if not pending:
            return {"published": 0, "checkpoint": after}
        total = 0
        for i in range(0, len(pending), max(1, chunk_size)):
            chunk = pending[i : i + max(1, chunk_size)]
            for sink in self.sinks:
                sink.publish(chunk)
            total += len(chunk) * max(1, len(self.sinks))
            offsets[consumer_group] = chunk[-1].commit_id
        self.checkpoints.write(offsets)
        return {"published": total, "checkpoint": offsets[consumer_group], "chunks": (len(pending) + chunk_size - 1) // chunk_size}
