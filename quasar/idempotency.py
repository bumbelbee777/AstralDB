"""Idempotent request deduplication for payment-style retries."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, TYPE_CHECKING

from quasar.security import atomic_write_json

if TYPE_CHECKING:
    from quasar.quasar import QuasarCluster

_IDEMPOTENCY_DDL = """
CREATE TABLE IF NOT EXISTS quasar_idempotency (
  idempotency_key TEXT PRIMARY KEY,
  status TEXT NOT NULL,
  response_digest TEXT,
  created_at TEXT NOT NULL
);
"""


@dataclass(frozen=True)
class IdempotencyConfig:
    table: str = "quasar_idempotency"
    require_key: bool = False
    store: str = "file"
    """``file`` (Quasar journal dir) or ``table`` (AstralDB table on routed shard)."""

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]]) -> "IdempotencyConfig":
        if not raw:
            return cls()
        return cls(
            table=str(raw.get("table", "quasar_idempotency")),
            require_key=bool(raw.get("require_key", False)),
            store=str(raw.get("store", "file")),
        )


def digest_result(payload: Any) -> str:
    text = json.dumps(payload, sort_keys=True, default=str)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:32]


class IdempotencyStore:
    """Tracks idempotency keys per routed shard (file store default for portability)."""

    def __init__(
        self,
        cluster: "QuasarCluster",
        config: IdempotencyConfig,
        *,
        base_dir: Path,
    ) -> None:
        self.cluster = cluster
        self.config = config
        self.base_dir = base_dir
        self.base_dir.mkdir(parents=True, exist_ok=True)
        self._bootstrapped = False

    def _shard_name(self, shard_key: str) -> str:
        return self.cluster.shard.node_for_key(shard_key).name

    def _file_path(self, shard_key: str, key: str) -> Path:
        digest = hashlib.sha256(key.encode("utf-8")).hexdigest()[:32]
        shard_dir = self.base_dir / self._shard_name(shard_key)
        shard_dir.mkdir(parents=True, exist_ok=True)
        return shard_dir / f"{digest}.json"

    def bootstrap(self) -> None:
        if self._bootstrapped or self.config.store != "table":
            return
        self.cluster.execute(_IDEMPOTENCY_DDL.strip())
        self._bootstrapped = True

    def get(self, key: str, *, shard_key: str) -> Optional[Dict[str, Any]]:
        if self.config.store == "table":
            self.bootstrap()
            from quasar.security import sql_literal_safe as sql_literal

            k = sql_literal(key)
            t = self.config.table
            results = self.cluster.execute(
                f"SELECT status, response_digest FROM {t} WHERE idempotency_key = {k};",
                shard_key=shard_key,
            )
            stdout = results[0].result.stdout if results else ""
            if "completed" in stdout.lower():
                return {"status": "completed", "response_digest": stdout[-32:]}
            if "pending" in stdout.lower():
                return {"status": "pending"}
            return None
        path = self._file_path(shard_key, key)
        if not path.is_file():
            return None
        return json.loads(path.read_text(encoding="utf-8"))

    def begin(self, key: str, *, shard_key: str) -> None:
        existing = self.get(key, shard_key=shard_key)
        if existing:
            status = existing.get("status")
            if status == "completed":
                raise RuntimeError(f"idempotency key already completed: {key}")
            if status == "pending":
                raise RuntimeError(f"idempotency key in flight: {key}")
        if self.config.store == "file":
            atomic_write_json(
                self._file_path(shard_key, key),
                {"status": "pending", "idempotency_key": key},
            )
            return
        self.bootstrap()
        from quasar.security import sql_literal_safe as sql_literal

        k = sql_literal(key)
        t = self.config.table
        self.cluster.execute(
            f"INSERT INTO {t} (idempotency_key, status, response_digest, created_at) "
            f"VALUES ({k}, 'pending', NULL, datetime('now'));",
            shard_key=shard_key,
        )

    def complete(self, key: str, *, shard_key: str, digest: str) -> None:
        if self.config.store == "file":
            atomic_write_json(
                self._file_path(shard_key, key),
                {"status": "completed", "idempotency_key": key, "response_digest": digest},
            )
            return
        from quasar.security import sql_literal_safe as sql_literal

        k, d = sql_literal(key), sql_literal(digest)
        t = self.config.table
        self.cluster.execute(
            f"UPDATE {t} SET status = 'completed', response_digest = {d} WHERE idempotency_key = {k};",
            shard_key=shard_key,
        )

    def fail(self, key: str, *, shard_key: str) -> None:
        if self.config.store == "file":
            atomic_write_json(
                self._file_path(shard_key, key),
                {"status": "failed", "idempotency_key": key},
            )
            return
        from quasar.security import sql_literal_safe as sql_literal

        k = sql_literal(key)
        t = self.config.table
        self.cluster.execute(
            f"UPDATE {t} SET status = 'failed' WHERE idempotency_key = {k};",
            shard_key=shard_key,
        )
