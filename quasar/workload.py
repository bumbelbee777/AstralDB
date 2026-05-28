"""Circuit breakers, adaptive limits, and workload stats for hot paths."""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Dict, Optional

from quasar.recovery import RetryPolicy, RetryStats, execute_with_retry


@dataclass
class CircuitBreakerConfig:
    failure_threshold: int = 5
    cooldown_sec: float = 30.0
    half_open_max_probes: int = 1


class ShardCircuitBreaker:
    """
    Per-shard circuit breaker with half-open probing after cooldown.

    Closed → open after ``failure_threshold`` failures; after cooldown, half-open
    allows up to ``half_open_max_probes`` trial requests before closing or reopening.
    """

    def __init__(self, config: Optional[CircuitBreakerConfig] = None) -> None:
        self.config = config or CircuitBreakerConfig()
        self._failures: Dict[str, int] = {}
        self._open_until: Dict[str, float] = {}
        self._half_open_probes: Dict[str, int] = {}
        self._lock = threading.Lock()

    def allow(self, shard_name: str) -> bool:
        with self._lock:
            now = time.time()
            until = self._open_until.get(shard_name, 0.0)
            if until and now < until:
                return False
            if until and now >= until:
                probes = self._half_open_probes.get(shard_name, 0)
                if probes >= self.config.half_open_max_probes:
                    return False
                self._half_open_probes[shard_name] = probes + 1
                return True
            return True

    def record_success(self, shard_name: str) -> None:
        with self._lock:
            self._failures[shard_name] = 0
            self._open_until.pop(shard_name, None)
            self._half_open_probes.pop(shard_name, None)

    def record_failure(self, shard_name: str) -> None:
        with self._lock:
            count = self._failures.get(shard_name, 0) + 1
            self._failures[shard_name] = count
            self._half_open_probes.pop(shard_name, None)
            if count >= self.config.failure_threshold:
                self._open_until[shard_name] = time.time() + self.config.cooldown_sec

    def snapshot(self) -> Dict[str, Dict[str, float]]:
        with self._lock:
            now = time.time()
            names = set(self._failures) | set(self._open_until) | set(self._half_open_probes)
            out: Dict[str, Dict[str, float]] = {}
            for name in names:
                until = self._open_until.get(name, 0.0)
                open_now = 1.0 if until and now < until else 0.0
                half_open = 1.0 if until and now >= until and open_now == 0.0 else 0.0
                out[name] = {
                    "failures": float(self._failures.get(name, 0)),
                    "open": open_now,
                    "half_open": half_open,
                    "half_open_probes": float(self._half_open_probes.get(name, 0)),
                }
            return out


@dataclass
class WorkloadStats:
    submitted: int = 0
    completed: int = 0
    failed: int = 0
    rejected_circuit: int = 0
    oltp_submitted: int = 0
    olap_submitted: int = 0
    lane_rejections: int = 0

    def to_dict(self) -> Dict[str, float]:
        return {
            "submitted": float(self.submitted),
            "completed": float(self.completed),
            "failed": float(self.failed),
            "rejected_circuit": float(self.rejected_circuit),
            "oltp_submitted": float(self.oltp_submitted),
            "olap_submitted": float(self.olap_submitted),
            "lane_rejections": float(self.lane_rejections),
        }


class WorkloadGuard:
    """Wraps shard execution with circuit breaking, retries, and counters."""

    def __init__(
        self,
        breaker: Optional[ShardCircuitBreaker] = None,
        *,
        retry_policy: Optional[RetryPolicy] = None,
    ) -> None:
        self.breaker = breaker or ShardCircuitBreaker()
        self.retry_policy = retry_policy or RetryPolicy(max_retries=0)
        self.stats = WorkloadStats()
        self.retry_stats = RetryStats()
        self._lock = threading.Lock()

    def check(self, shard_name: str) -> bool:
        if not self.breaker.allow(shard_name):
            with self._lock:
                self.stats.rejected_circuit += 1
            return False
        with self._lock:
            self.stats.submitted += 1
        return True

    def record_lane(self, lane: str) -> None:
        with self._lock:
            if lane == "olap":
                self.stats.olap_submitted += 1
            else:
                self.stats.oltp_submitted += 1

    def reject_lane(self) -> None:
        with self._lock:
            self.stats.lane_rejections += 1

    def run(self, shard_name: str, fn):
        """Execute ``fn()`` with optional transient retries and circuit accounting."""
        if not self.check(shard_name):
            from quasar.errors import QuasarCircuitOpenError

            raise QuasarCircuitOpenError(f"circuit breaker open for shard: {shard_name}")

        def _call():
            return fn()

        try:
            if self.retry_policy.max_retries > 0:
                result = execute_with_retry(_call, self.retry_policy, stats=self.retry_stats)
            else:
                result = _call()
        except Exception as exc:
            self.failure(shard_name)
            raise

        self.success(shard_name)
        return result

    def success(self, shard_name: str) -> None:
        self.breaker.record_success(shard_name)
        with self._lock:
            self.stats.completed += 1

    def failure(self, shard_name: str) -> None:
        self.breaker.record_failure(shard_name)
        with self._lock:
            self.stats.failed += 1
