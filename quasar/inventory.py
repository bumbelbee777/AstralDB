"""Cluster inventory: disk usage and file metadata per shard."""

from __future__ import annotations

import hashlib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

from quasar.quasar import ShardNode


@dataclass
class FileStats:
    path: str
    exists: bool
    bytes: int
    wal_bytes: int

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


def _file_size(path: Path) -> int:
    return path.stat().st_size if path.is_file() else 0


def stats_for_node(node: ShardNode) -> FileStats:
    db = node.database
    wal = node.wal_path()
    return FileStats(
        path=str(db),
        exists=db.is_file(),
        bytes=_file_size(db),
        wal_bytes=_file_size(wal),
    )


def sha256_file(path: Path, *, chunk_size: int = 1 << 20) -> Optional[str]:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


class QuasarInventory:
    """Report sizes and optional content hashes for shard databases."""

    def __init__(self, nodes: Sequence[ShardNode]) -> None:
        self.nodes = list(nodes)

    def collect(self, *, hash_files: bool = False) -> Dict[str, Any]:
        shards: List[Dict[str, Any]] = []
        total_db = 0
        total_wal = 0
        for node in self.nodes:
            st = stats_for_node(node)
            total_db += st.bytes
            total_wal += st.wal_bytes
            entry: Dict[str, Any] = {"name": node.name, **st.to_dict()}
            if hash_files:
                entry["sha256"] = sha256_file(node.database)
                entry["wal_sha256"] = sha256_file(node.wal_path())
            shards.append(entry)
        return {
            "shards": shards,
            "total_db_bytes": total_db,
            "total_wal_bytes": total_wal,
            "total_bytes": total_db + total_wal,
        }
