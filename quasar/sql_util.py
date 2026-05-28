"""Shared SQL text helpers."""

from __future__ import annotations

from typing import List


def split_sql_script(text: str) -> List[str]:
    """Split a SQL file into statements (respects semicolons outside quotes)."""
    parts: List[str] = []
    buf: List[str] = []
    in_single = False
    in_double = False
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "'" and not in_double:
            in_single = not in_single
            buf.append(ch)
        elif ch == '"' and not in_single:
            in_double = not in_double
            buf.append(ch)
        elif ch == ";" and not in_single and not in_double:
            stmt = "".join(buf).strip()
            if stmt and not stmt.startswith("--"):
                parts.append(stmt)
            buf = []
        else:
            buf.append(ch)
        i += 1
    tail = "".join(buf).strip()
    if tail and not tail.startswith("--"):
        parts.append(tail)
    return parts
