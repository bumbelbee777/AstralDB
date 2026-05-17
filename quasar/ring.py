"""Consistent hashing for shard routing."""

from __future__ import annotations

import hashlib
from typing import Dict, List, Optional


class ConsistentHashRing:
    """MD5-based consistent hash ring with virtual nodes."""

    def __init__(self, nodes: List[str], virtual_nodes: int = 128) -> None:
        if not nodes:
            raise ValueError("at least one node is required")
        self.virtual_nodes = virtual_nodes
        self.ring: Dict[int, str] = {}
        for node in nodes:
            for replica in range(virtual_nodes):
                digest = hashlib.md5(f"{node}:{replica}".encode(), usedforsecurity=False).hexdigest()
                self.ring[int(digest, 16)] = node
        self._sorted_keys = sorted(self.ring.keys())

    def node_for_key(self, key: str) -> str:
        if not self._sorted_keys:
            raise ValueError("empty hash ring")
        digest = hashlib.md5(key.encode(), usedforsecurity=False).hexdigest()
        point = int(digest, 16)
        for ring_point in self._sorted_keys:
            if point <= ring_point:
                return self.ring[ring_point]
        return self.ring[self._sorted_keys[0]]

    def nodes_for_keys(self, keys: List[str]) -> List[str]:
        return [self.node_for_key(k) for k in keys]

    @property
    def nodes(self) -> List[str]:
        return sorted({self.ring[k] for k in self._sorted_keys})
