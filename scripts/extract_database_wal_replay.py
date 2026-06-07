#!/usr/bin/env python3
"""Extract Database::ReplayWal* method bodies into DatabaseWalReplay.cxx."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DB_CXX = ROOT / "sources/Database/Database.cxx"
OUT = ROOT / "sources/Database/DatabaseWalReplay.cxx"

HDR = """\
#include <Database/Database.hxx>
#include <Database/PtBridge/PtBridge.hxx>
#include <Database/Graph/GraphStorage.hxx>
#include <Database/Embedding/EmbeddingStorage.hxx>

namespace AstralDB {
"""

FTR = """
} // namespace AstralDB
"""


def extract_replaywal_functions(text: str) -> list[str]:
    pattern = re.compile(
        r"^void Database::ReplayWal[A-Za-z0-9_]+\([^{]*\)\s*\{",
        re.M,
    )
    out: list[str] = []
    for m in pattern.finditer(text):
        start = m.start()
        brace = text.find("{", m.end() - 1)
        depth = 0
        i = brace
        while i < len(text):
            c = text[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    out.append(text[start : i + 1])
                    break
            i += 1
    return out


def remove_functions(text: str, blocks: list[str]) -> str:
    for block in blocks:
        text = text.replace(block + "\n", "", 1)
        text = text.replace(block, "", 1)
    return text


def main() -> None:
    text = DB_CXX.read_text(encoding="utf-8")
    blocks = extract_replaywal_functions(text)
    if not blocks:
        raise SystemExit("No ReplayWal functions found")
    OUT.write_text(HDR + "\n\n".join(blocks) + FTR, encoding="utf-8")
    DB_CXX.write_text(remove_functions(text, blocks), encoding="utf-8")
    print(f"Extracted {len(blocks)} ReplayWal functions")


if __name__ == "__main__":
    main()
