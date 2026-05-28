"""Serverless worker lease/heartbeat and scale-to-zero controls."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict


@dataclass
class LeaseState:
    generation: int = 0
    holder: str = "none"
    heartbeat_unix: float = 0.0
    active: bool = False

    def to_dict(self) -> Dict[str, Any]:
        return {
            "generation": self.generation,
            "holder": self.holder,
            "heartbeat_unix": self.heartbeat_unix,
            "active": self.active,
        }


class QuasarServerlessController:
    def __init__(self, state_path: Path, *, idle_sec: float = 30.0) -> None:
        self.state_path = state_path
        self.idle_sec = idle_sec
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state = LeaseState()
        self._save()

    def _save(self) -> None:
        self.state_path.write_text(json.dumps(self.state.to_dict(), indent=2) + "\n", encoding="utf-8")

    def acquire(self, holder: str) -> Dict[str, Any]:
        self.state.generation += 1
        self.state.holder = holder
        self.state.heartbeat_unix = time.time()
        self.state.active = True
        self._save()
        return self.state.to_dict()

    def heartbeat(self, holder: str) -> Dict[str, Any]:
        if self.state.holder == holder and self.state.active:
            self.state.heartbeat_unix = time.time()
            self._save()
        return self.state.to_dict()

    def tick(self) -> Dict[str, Any]:
        now = time.time()
        if self.state.active and (now - self.state.heartbeat_unix) > self.idle_sec:
            self.state.active = False
            self._save()
        return self.state.to_dict()
