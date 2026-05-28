"""Binary-framed IPC helpers for local Quasar components."""

from __future__ import annotations

import json
import struct
from typing import Any, Dict, Iterable, List


def encode_frames(messages: Iterable[Dict[str, Any]]) -> bytes:
    out = bytearray()
    for msg in messages:
        payload = json.dumps(msg, separators=(",", ":")).encode("utf-8")
        out.extend(struct.pack(">I", len(payload)))
        out.extend(payload)
    return bytes(out)


def decode_frames(blob: bytes) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    i = 0
    n = len(blob)
    while i + 4 <= n:
        size = struct.unpack(">I", blob[i : i + 4])[0]
        i += 4
        if i + size > n:
            break
        raw = blob[i : i + size]
        i += size
        try:
            obj = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            out.append(obj)
    return out
