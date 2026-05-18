"""Transient error detection, retries, and subprocess recovery helpers."""

from __future__ import annotations

import random
import time
from dataclasses import dataclass
from typing import Any, Callable, Dict, Optional, TypeVar

from quasar.errors import QuasarOverloadError

T = TypeVar("T")

_TRANSIENT_MARKERS = (
    "timeout",
    "timed out",
    "temporarily unavailable",
    "resource busy",
    "busy",
    "locked",
    "lock",
    "connection reset",
    "pool queue full",
    "overloaded",
    "try again",
    "snapshot",
    "wal",
)


@dataclass(frozen=True)
class RetryPolicy:
    max_retries: int = 3
    base_delay_ms: float = 50.0
    max_delay_ms: float = 2000.0
    jitter: bool = True

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]]) -> "RetryPolicy":
        if not raw:
            return cls()
        return cls(
            max_retries=max(0, min(int(raw.get("max_retries", 3)), 20)),
            base_delay_ms=max(0.0, float(raw.get("base_delay_ms", 50.0))),
            max_delay_ms=max(0.0, float(raw.get("max_delay_ms", 2000.0))),
            jitter=bool(raw.get("jitter", True)),
        )


@dataclass
class RetryStats:
    attempts: int = 0
    retries: int = 0
    successes_after_retry: int = 0
    exhausted: int = 0

    def to_dict(self) -> Dict[str, float]:
        return {
            "attempts": float(self.attempts),
            "retries": float(self.retries),
            "successes_after_retry": float(self.successes_after_retry),
            "exhausted": float(self.exhausted),
        }


def is_transient_error(exc: BaseException) -> bool:
    """Classify errors that may succeed on retry (timeouts, overload, I/O blips)."""
    if isinstance(exc, QuasarOverloadError):
        return True
    text = str(exc).lower()
    return any(marker in text for marker in _TRANSIENT_MARKERS)


def retry_delay_ms(attempt: int, policy: RetryPolicy) -> float:
    """Exponential backoff with optional jitter (attempt is 1-based)."""
    delay = min(policy.max_delay_ms, policy.base_delay_ms * (2 ** max(0, attempt - 1)))
    if policy.jitter and delay > 0:
        delay *= 0.5 + random.random()
    return delay


def execute_with_retry(
    fn: Callable[[], T],
    policy: RetryPolicy,
    *,
    stats: Optional[RetryStats] = None,
    is_retryable: Callable[[BaseException], bool] = is_transient_error,
) -> T:
    """Run ``fn`` until success or retries are exhausted."""
    last_exc: Optional[BaseException] = None
    for attempt in range(policy.max_retries + 1):
        if stats is not None:
            stats.attempts += 1
        try:
            result = fn()
            if stats is not None and attempt > 0:
                stats.successes_after_retry += 1
            return result
        except Exception as exc:
            last_exc = exc
            if attempt >= policy.max_retries or not is_retryable(exc):
                if stats is not None and attempt > 0:
                    stats.exhausted += 1
                raise
            if stats is not None:
                stats.retries += 1
            time.sleep(retry_delay_ms(attempt + 1, policy) / 1000.0)
    assert last_exc is not None
    raise last_exc


def rollback_database(client: Any, database: Any) -> None:
    """Best-effort ``ROLLBACK`` after a failed batch or transaction."""
    try:
        client.query("ROLLBACK;", database=database, immediate=True)
    except Exception:
        pass
