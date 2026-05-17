"""Circuit breakers, adaptive limits, and workload stats for hot paths."""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Dict, Optional


@dataclass
class CircuitBreakerConfig:
    failure_threshold: int = 5
    cooldown_sec: float = 30.0


class ShardCircuitBreaker:
    def __init__(self, config: Optional[CircuitBreakerConfig] = None) -> None:
        self.config = config or CircuitBreakerConfig()
        self._failures: Dict[str, int] = {}
        self._open_until: Dict[str, float] = {}
        self._lock = threading.Lock()

    def allow(self, shard_name: str) -> bool:
        with self._lock:
            until = self._open_until.get(shard_name, 0)
            if time.time() < until:
                return False
            return True

    def record_success(self, shard_name: str) -> None:
        with self._lock:
            self._failures[shard_name] = 0
            self._open_until.pop(shard_name, None)

    def record_failure(self, shard_name: str) -> None:
        with self._lock:
            count = self._failures.get(shard_name, 0) + 1
            self._failures[shard_name] = count
            if count >= self.config.failure_threshold:
                self._open_until[shard_name] = time.time() + self.config.cooldown_sec

    def snapshot(self) -> Dict[str, Dict[str, float]]:
        with self._lock:
            now = time.time()
            return {
                name: {
                    "failures": float(self._failures.get(name, 0)),
                    "open": 1.0 if now < self._open_until.get(name, 0) else 0.0,
                }
                for name in set(self._failures) | set(self._open_until)
            }


@dataclass
class WorkloadStats:
    submitted: int = 0
    completed: int = 0
    failed: int = 0
    rejected_circuit: int = 0

    def to_dict(self) -> Dict[str, float]:
        return {
            "submitted": float(self.submitted),
            "completed": float(self.completed),
            "failed": float(self.failed),
            "rejected_circuit": float(self.rejected_circuit),
        }


class WorkloadGuard:
    """Wraps shard execution with circuit breaking and counters."""

    def __init__(self, breaker: Optional[ShardCircuitBreaker] = None) -> None:
        self.breaker = breaker or ShardCircuitBreaker()
        self.stats = WorkloadStats()
        self._lock = threading.Lock()

    def check(self, shard_name: str) -> bool:
        if not self.breaker.allow(shard_name):
            with self._lock:
                self.stats.rejected_circuit += 1
            return False
        with self._lock:
            self.stats.submitted += 1
        return True

    def success(self, shard_name: str) -> None:
        self.breaker.record_success(shard_name)
        with self._lock:
            self.stats.completed += 1

    def failure(self, shard_name: str) -> None:
        self.breaker.record_failure(shard_name)
        with self._lock:
            self.stats.failed += 1
