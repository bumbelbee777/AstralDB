"""
Production hardening: input validation, path containment, and safe defaults.

Used across Quasar before subprocess invocation, config load, and HTTP gateway handling.
"""

from __future__ import annotations

import hmac
import json
import os
import re
import secrets
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Union

from quasar.errors import QuasarSecurityError
from quasar.paths import resolve_cluster_path, resolve_path_under_base

PathLike = Union[str, Path]

# Backwards-compatible alias
SecurityError = QuasarSecurityError

# ---------------------------------------------------------------------------
# Limits (override via cluster.json "security" section)
# ---------------------------------------------------------------------------
IDENTIFIER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,127}$")
SHARD_NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,63}$")
REGION_NAME_RE = SHARD_NAME_RE
BACKUP_VERSION_RE = re.compile(r"^[A-Za-z0-9_.-]{1,128}$")


@dataclass(frozen=True)
class SecurityPolicy:
    max_sql_bytes: int = 512 * 1024
    max_config_bytes: int = 2 * 1024 * 1024
    max_shard_key_bytes: int = 4096
    max_pool_queue: int = 10_000
    max_gateway_body_bytes: int = 1024 * 1024
    gateway_rate_per_minute: int = 1200
    require_gateway_auth: bool = False
    gateway_allow_query_api_key: bool = False
    gateway_keys_file: Optional[str] = None
    redact_secrets_in_errors: bool = True
    allow_path_outside_config_root: bool = False

    @classmethod
    def from_config(cls, section: Optional[Dict[str, Any]]) -> "SecurityPolicy":
        if not section:
            return cls()
        known = {f.name for f in cls.__dataclass_fields__.values()}  # type: ignore[attr-defined]
        kwargs = {k: section[k] for k in section if k in known}
        return cls(**kwargs)


def policy_from_env() -> SecurityPolicy:
    """Optional env overrides for operators (QUASAR_MAX_SQL_BYTES, etc.)."""
    base = SecurityPolicy()
    overrides: Dict[str, Any] = {}
    if v := os.environ.get("QUASAR_MAX_SQL_BYTES"):
        overrides["max_sql_bytes"] = int(v)
    if v := os.environ.get("QUASAR_REQUIRE_GATEWAY_AUTH"):
        overrides["require_gateway_auth"] = v.lower() in ("1", "true", "yes")
    return SecurityPolicy(**{**base.__dict__, **overrides}) if overrides else base


def constant_time_equal(provided: Optional[str], expected: Optional[str]) -> bool:
    if expected is None or provided is None:
        return False
    return hmac.compare_digest(provided.encode("utf-8"), expected.encode("utf-8"))


def validate_shard_name(name: str) -> str:
    if not SHARD_NAME_RE.fullmatch(name):
        raise SecurityError(f"invalid shard name: {name!r}")
    return name


def validate_sql_identifier(name: str, *, label: str = "identifier") -> str:
    if not IDENTIFIER_RE.fullmatch(name):
        raise SecurityError(f"invalid {label}: {name!r}")
    return name


def validate_sql(sql: str, policy: Optional[SecurityPolicy] = None) -> str:
    policy = policy or SecurityPolicy()
    if not isinstance(sql, str):
        raise SecurityError("sql must be a string")
    if "\x00" in sql:
        raise SecurityError("sql contains null bytes")
    encoded = sql.encode("utf-8")
    if len(encoded) > policy.max_sql_bytes:
        raise SecurityError(f"sql exceeds max size ({policy.max_sql_bytes} bytes)")
    return sql


def validate_shard_key(key: str, policy: Optional[SecurityPolicy] = None) -> str:
    policy = policy or SecurityPolicy()
    if not isinstance(key, str) or not key.strip():
        raise SecurityError("shard_key must be a non-empty string")
    if len(key.encode("utf-8")) > policy.max_shard_key_bytes:
        raise SecurityError("shard_key too long")
    if "\x00" in key or "\n" in key or "\r" in key:
        raise SecurityError("shard_key contains illegal characters")
    return key


def validate_executable(path: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_file():
        raise SecurityError(f"executable not found: {resolved}")
    if os.access(resolved, os.X_OK) is False and os.name != "nt":
        raise SecurityError(f"executable not executable: {resolved}")
    return resolved


def load_json_config_file(path: Path, policy: Optional[SecurityPolicy] = None) -> Dict[str, Any]:
    policy = policy or SecurityPolicy()
    data = path.read_bytes()
    if len(data) > policy.max_config_bytes:
        raise SecurityError(f"config file exceeds {policy.max_config_bytes} bytes")
    return json.loads(data.decode("utf-8"))


def atomic_write_json(path: Path, payload: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    tmp.replace(path)


def redact_command(cmd: list[str]) -> list[str]:
    out: list[str] = []
    skip = False
    for part in cmd:
        if skip:
            out.append("***")
            skip = False
            continue
        if part in ("-P", "--password"):
            out.append(part)
            skip = True
            continue
        out.append(part)
    return out


def sql_literal_safe(value: Any) -> str:
    """Escape string literals for generated SQL (rebalance). Numbers only for numeric types."""
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int) and not isinstance(value, bool):
        return str(value)
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):  # NaN / inf
            return "NULL"
        return repr(float(value))
    text = str(value).replace("'", "''")
    if len(text) > 64 * 1024:
        raise SecurityError("string literal too large for generated SQL")
    return f"'{text}'"


def clamp_pool_config(raw: Dict[str, Any]) -> Dict[str, Any]:
    """Enforce sane bounds on pool tuning."""
    out = dict(raw)
    out["max_workers"] = max(1, min(int(out.get("max_workers", 32)), 256))
    out["batch_max_statements"] = max(1, min(int(out.get("batch_max_statements", 48)), 500))
    out["batch_window_ms"] = max(0.5, min(float(out.get("batch_window_ms", 3.0)), 1000.0))
    out["max_inflight_batches"] = max(1, min(int(out.get("max_inflight_batches", 24)), 256))
    out["max_queue"] = max(100, min(int(out.get("max_queue", 10_000)), 500_000))
    if "per_db_max_inflight" in out:
        out["per_db_max_inflight"] = max(1, min(int(out["per_db_max_inflight"]), 64))
    if "keepalive_interval_sec" in out:
        out["keepalive_interval_sec"] = max(0.0, min(float(out["keepalive_interval_sec"]), 3600.0))
    if "overload_soft_limit_ratio" in out:
        out["overload_soft_limit_ratio"] = max(0.5, min(float(out["overload_soft_limit_ratio"]), 0.99))
    if "overload_hard_limit_ratio" in out:
        out["overload_hard_limit_ratio"] = max(0.6, min(float(out["overload_hard_limit_ratio"]), 1.0))
    if out.get("overload_hard_limit_ratio", 0.95) < out.get("overload_soft_limit_ratio", 0.75):
        out["overload_hard_limit_ratio"] = out["overload_soft_limit_ratio"]
    if "overload_mode" in out:
        mode = str(out["overload_mode"]).strip().lower()
        out["overload_mode"] = mode if mode in ("fail_fast", "degrade") else "fail_fast"
    return out


class RateLimiter:
    """Simple per-key requests-per-minute limiter for the HTTP gateway."""

    def __init__(self, per_minute: int) -> None:
        self.per_minute = max(1, per_minute)
        self._buckets: Dict[str, list[float]] = {}

    def allow(self, key: str) -> bool:
        import time

        now = time.time()
        window = 60.0
        bucket = self._buckets.setdefault(key, [])
        self._buckets[key] = [t for t in bucket if now - t < window]
        if len(self._buckets[key]) >= self.per_minute:
            return False
        self._buckets[key].append(now)
        return True
