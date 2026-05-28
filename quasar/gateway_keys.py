"""Hashed gateway API keys with rotation, scopes, and constant-time verification."""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import secrets
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from quasar.security import atomic_write_json, constant_time_equal

KEY_PREFIX = "qk_"
PBKDF2_ITERATIONS = 600_000
DEFAULT_SCOPES = ("gateway", "query", "metrics")


@dataclass
class GatewayApiKeyRecord:
    key_id: str
    secret_hash: str
    salt: str
    scopes: List[str] = field(default_factory=lambda: list(DEFAULT_SCOPES))
    created_at: float = field(default_factory=time.time)
    expires_at: Optional[float] = None
    enabled: bool = True
    description: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, raw: Dict[str, Any]) -> "GatewayApiKeyRecord":
        return cls(
            key_id=str(raw["key_id"]),
            secret_hash=str(raw["secret_hash"]),
            salt=str(raw["salt"]),
            scopes=list(raw.get("scopes") or list(DEFAULT_SCOPES)),
            created_at=float(raw.get("created_at", time.time())),
            expires_at=float(raw["expires_at"]) if raw.get("expires_at") is not None else None,
            enabled=bool(raw.get("enabled", True)),
            description=str(raw.get("description", "")),
        )


def hash_gateway_secret(secret: str, salt: bytes, *, iterations: int = PBKDF2_ITERATIONS) -> str:
    digest = hashlib.pbkdf2_hmac("sha256", secret.encode("utf-8"), salt, iterations)
    return base64.b64encode(digest).decode("ascii")


def generate_gateway_secret() -> str:
    return secrets.token_urlsafe(32)


def format_api_token(key_id: str, secret: str) -> str:
    return f"{KEY_PREFIX}{key_id}.{secret}"


def parse_api_token(token: str) -> Tuple[str, str]:
    if not token.startswith(KEY_PREFIX):
        raise ValueError("token must start with qk_")
    body = token[len(KEY_PREFIX) :]
    if "." not in body:
        raise ValueError("token must be qk_<id>.<secret>")
    key_id, secret = body.split(".", 1)
    if not key_id or not secret:
        raise ValueError("invalid token shape")
    return key_id, secret


def extract_bearer_token(authorization: Optional[str]) -> Optional[str]:
    if not authorization:
        return None
    parts = authorization.strip().split(None, 1)
    if len(parts) != 2 or parts[0].lower() != "bearer":
        return None
    return parts[1].strip() or None


class GatewayKeyStore:
    """Load and verify hashed API keys from a JSON registry file."""

    def __init__(self, path: Path, *, iterations: int = PBKDF2_ITERATIONS) -> None:
        self.path = path.resolve()
        self.iterations = iterations
        self._records: Dict[str, GatewayApiKeyRecord] = {}
        if self.path.is_file():
            self.reload()

    def reload(self) -> None:
        raw = json.loads(self.path.read_text(encoding="utf-8"))
        keys = raw.get("keys", raw) if isinstance(raw, dict) else raw
        self._records = {}
        if isinstance(keys, list):
            for item in keys:
                rec = GatewayApiKeyRecord.from_dict(item)
                self._records[rec.key_id] = rec
        elif isinstance(keys, dict):
            for key_id, item in keys.items():
                item = dict(item)
                item.setdefault("key_id", key_id)
                rec = GatewayApiKeyRecord.from_dict(item)
                self._records[rec.key_id] = rec

    def save(self) -> None:
        payload = {
            "version": 1,
            "iterations": self.iterations,
            "keys": [r.to_dict() for r in sorted(self._records.values(), key=lambda x: x.key_id)],
        }
        atomic_write_json(self.path, payload)

    def list_keys(self) -> List[Dict[str, Any]]:
        out: List[Dict[str, Any]] = []
        for rec in self._records.values():
            out.append(
                {
                    "key_id": rec.key_id,
                    "scopes": list(rec.scopes),
                    "enabled": rec.enabled,
                    "created_at": rec.created_at,
                    "expires_at": rec.expires_at,
                    "description": rec.description,
                }
            )
        return out

    def create_key(
        self,
        *,
        scopes: Optional[List[str]] = None,
        description: str = "",
        expires_in_sec: Optional[float] = None,
        key_id: Optional[str] = None,
    ) -> Tuple[str, str]:
        kid = key_id or secrets.token_hex(8)
        if kid in self._records:
            raise ValueError(f"key_id already exists: {kid}")
        secret = generate_gateway_secret()
        salt = secrets.token_bytes(16)
        rec = GatewayApiKeyRecord(
            key_id=kid,
            secret_hash=hash_gateway_secret(secret, salt, iterations=self.iterations),
            salt=base64.b64encode(salt).decode("ascii"),
            scopes=list(scopes or DEFAULT_SCOPES),
            description=description,
            expires_at=(time.time() + expires_in_sec) if expires_in_sec else None,
        )
        self._records[kid] = rec
        self.save()
        return format_api_token(kid, secret), kid

    def revoke_key(self, key_id: str) -> bool:
        if key_id not in self._records:
            return False
        self._records[key_id].enabled = False
        self.save()
        return True

    def delete_key(self, key_id: str) -> bool:
        if key_id not in self._records:
            return False
        del self._records[key_id]
        self.save()
        return True

    def verify_token(self, token: Optional[str]) -> Optional[GatewayApiKeyRecord]:
        if not token:
            return None
        try:
            key_id, secret = parse_api_token(token)
        except ValueError:
            return None
        rec = self._records.get(key_id)
        if rec is None or not rec.enabled:
            return None
        if rec.expires_at is not None and time.time() > rec.expires_at:
            return None
        try:
            salt = base64.b64decode(rec.salt.encode("ascii"))
        except (ValueError, OSError):
            return None
        expected = rec.secret_hash
        actual = hash_gateway_secret(secret, salt, iterations=self.iterations)
        if not hmac.compare_digest(actual.encode("utf-8"), expected.encode("utf-8")):
            return None
        return rec

    def verify_legacy_plaintext(self, provided: Optional[str], expected: Optional[str]) -> bool:
        return constant_time_equal(provided, expected)


def resolve_gateway_credential(
    *,
    headers: Dict[str, str],
    query_args: Dict[str, str],
    legacy_plaintext: Optional[str],
    key_store: Optional[GatewayKeyStore],
    allow_query_api_key: bool,
    require_auth: bool = False,
) -> Tuple[bool, Optional[str]]:
    """Return (authorized, key_id_or_legacy)."""
    auth_header = headers.get("Authorization") or headers.get("authorization")
    bearer = extract_bearer_token(auth_header)
    header_key = headers.get("X-Quasar-Key") or headers.get("x-quasar-key")
    candidates: List[str] = []
    for item in (bearer, header_key):
        if item:
            candidates.append(item)
    if allow_query_api_key:
        q = query_args.get("api_key")
        if q:
            candidates.append(q)

    has_auth_config = bool(legacy_plaintext) or key_store is not None
    if not has_auth_config and not require_auth:
        return True, None

    if not candidates:
        return (not require_auth), None

    if key_store is not None:
        for cand in candidates:
            rec = key_store.verify_token(cand)
            if rec is not None:
                return True, rec.key_id

    if legacy_plaintext:
        for cand in candidates:
            if constant_time_equal(cand, legacy_plaintext):
                return True, "legacy"

    return False, None
