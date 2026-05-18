"""Quasar operation journal and AstralDB per-shard audit file routing."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional


@dataclass
class AuditEvent:
    ts: str
    event: str
    actor: str
    detail: str
    outcome: str
    shard: Optional[str] = None
    idempotency_key: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


class QuasarJournal:
    """Append-only JSONL journal for Quasar-orchestrated operations (complements AstralDB audit)."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def record(
        self,
        event: str,
        *,
        actor: str = "quasar",
        detail: str = "",
        outcome: str = "OK",
        shard: Optional[str] = None,
        idempotency_key: Optional[str] = None,
        extra: Optional[Dict[str, Any]] = None,
    ) -> AuditEvent:
        row = AuditEvent(
            ts=datetime.now(timezone.utc).isoformat(),
            event=event,
            actor=actor,
            detail=detail,
            outcome=outcome,
            shard=shard,
            idempotency_key=idempotency_key,
        )
        payload = row.to_dict()
        if extra:
            payload["extra"] = extra
        with self.path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(payload, separators=(",", ":")) + "\n")
        return row

    def tail(self, limit: int = 100) -> List[Dict[str, Any]]:
        if not self.path.is_file():
            return []
        lines = self.path.read_text(encoding="utf-8").splitlines()
        out: List[Dict[str, Any]] = []
        for line in lines[-limit:]:
            line = line.strip()
            if line:
                out.append(json.loads(line))
        return out


def shard_audit_path(audit_dir: Path, shard_name: str) -> Path:
    """Per-shard AstralDB ``--audit-file`` path."""
    audit_dir.mkdir(parents=True, exist_ok=True)
    return audit_dir / f"{shard_name}.audit.log"
