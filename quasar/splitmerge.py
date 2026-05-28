"""Automatic shard split/merge controller with movement journal."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional
from quasar.binary_ipc import decode_frames, encode_frames


@dataclass
class MovementRecord:
    ts: float
    action: str
    details: Dict[str, Any]

    def to_dict(self) -> Dict[str, Any]:
        return {"ts": self.ts, "action": self.action, "details": self.details}


class SplitMergeJournal:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if not self.path.exists():
            self.path.write_text("[]", encoding="utf-8")

    def append(self, rec: MovementRecord) -> None:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            data = []
        if not isinstance(data, list):
            data = []
        data.append(rec.to_dict())
        self.path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    def read(self) -> List[Dict[str, Any]]:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return []
        return data if isinstance(data, list) else []

    def append_binary_batch(self, records: List[MovementRecord]) -> None:
        bin_path = self.path.with_suffix(self.path.suffix + ".bin")
        payload = encode_frames([r.to_dict() for r in records])
        with bin_path.open("ab") as fh:
            fh.write(payload)

    def read_binary(self) -> List[Dict[str, Any]]:
        bin_path = self.path.with_suffix(self.path.suffix + ".bin")
        if not bin_path.exists():
            return []
        return decode_frames(bin_path.read_bytes())


class QuasarSplitMergeController:
    def __init__(self, cluster, *, journal_path: Path) -> None:
        self.cluster = cluster
        self.journal = SplitMergeJournal(journal_path)

    def split_shard(self, shard_name: str, *, split_key: str) -> Dict[str, Any]:
        plan = {"action": "split", "shard": shard_name, "split_key": split_key}
        self.journal.append(MovementRecord(ts=time.time(), action="split_plan", details=plan))
        return {"planned": True, **plan}

    def split_shard_chunked(
        self, shard_name: str, *, split_key: str, chunk_size: int = 100, total_rows: int = 1000
    ) -> Dict[str, Any]:
        rows = max(1, total_rows)
        csize = max(1, chunk_size)
        recs: List[MovementRecord] = []
        moved = 0
        idx = 0
        while moved < rows:
            take = min(csize, rows - moved)
            details = {"shard": shard_name, "split_key": split_key, "chunk_idx": idx, "rows": take}
            recs.append(MovementRecord(ts=time.time(), action="split_chunk", details=details))
            moved += take
            idx += 1
        self.journal.append_binary_batch(recs)
        return {"planned": True, "chunks": len(recs), "rows": rows}

    def merge_shards(self, sources: List[str], *, target: str) -> Dict[str, Any]:
        plan = {"action": "merge", "sources": sources, "target": target}
        self.journal.append(MovementRecord(ts=time.time(), action="merge_plan", details=plan))
        return {"planned": True, **plan}

    def merge_shards_chunked(
        self, sources: List[str], *, target: str, chunk_size: int = 100, total_rows: int = 1000
    ) -> Dict[str, Any]:
        rows = max(1, total_rows)
        csize = max(1, chunk_size)
        recs: List[MovementRecord] = []
        moved = 0
        idx = 0
        while moved < rows:
            take = min(csize, rows - moved)
            details = {"sources": sources, "target": target, "chunk_idx": idx, "rows": take}
            recs.append(MovementRecord(ts=time.time(), action="merge_chunk", details=details))
            moved += take
            idx += 1
        self.journal.append_binary_batch(recs)
        return {"planned": True, "chunks": len(recs), "rows": rows}

    def tick(self) -> Dict[str, Any]:
        return {"journal_records": len(self.journal.read()), "binary_records": len(self.journal.read_binary())}
