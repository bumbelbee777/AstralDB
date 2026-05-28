"""Edge node registry and CDC-driven sync scaffolding."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List


@dataclass
class EdgeNode:
    node_id: str
    region: str
    last_sync_commit: int = 0
    last_seen_unix: float = 0.0
    vector_clock: Dict[str, int] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.vector_clock is None:
            self.vector_clock = {}

    def to_dict(self) -> Dict[str, Any]:
        return {
            "node_id": self.node_id,
            "region": self.region,
            "last_sync_commit": self.last_sync_commit,
            "last_seen_unix": self.last_seen_unix,
            "vector_clock": self.vector_clock,
        }


class EdgeRegistry:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if not self.path.exists():
            self.path.write_text("{}", encoding="utf-8")

    def _read(self) -> Dict[str, Any]:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            data = {}
        return data if isinstance(data, dict) else {}

    def _write(self, data: Dict[str, Any]) -> None:
        self.path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    def register(self, node_id: str, region: str) -> Dict[str, Any]:
        data = self._read()
        now = time.time()
        data[node_id] = EdgeNode(node_id=node_id, region=region, last_seen_unix=now).to_dict()
        self._write(data)
        return data[node_id]

    def sync(self, node_id: str, events: List[Dict[str, Any]]) -> Dict[str, Any]:
        data = self._read()
        node = data.get(node_id)
        if not isinstance(node, dict):
            raise RuntimeError("edge node not registered")
        max_commit = max([int(e.get("commit_id", 0)) for e in events], default=int(node.get("last_sync_commit", 0)))
        node["last_sync_commit"] = max_commit
        node["last_seen_unix"] = time.time()
        vc = dict(node.get("vector_clock", {}))
        vc["core"] = max(int(vc.get("core", 0)), max_commit)
        node["vector_clock"] = vc
        data[node_id] = node
        self._write(data)
        return {"node": node_id, "applied_events": len(events), "last_sync_commit": max_commit}

    def list_nodes(self) -> Dict[str, Any]:
        return self._read()
