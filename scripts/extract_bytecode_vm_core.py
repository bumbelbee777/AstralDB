#!/usr/bin/env python3
"""Move BytecodeInterpreter stack/session methods into BytecodeVmCore.cxx."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BC = ROOT / "sources/SQL/Bytecode/Bytecode.cxx"
OUT = ROOT / "sources/SQL/Bytecode/BytecodeVmCore.cxx"

METHODS = [
    "CleanupStack",
    "PushScalarWord",
    "PushOwningStringHeap",
    "PopScalarWord",
    "PopDiscardTopSlot",
    "ResetVmState",
    "ResetExecutionSession",
    "EnsurePrimaryDatabaseOpened",
    "ReloadPrimaryDatabaseFromDisk",
    "RestorePrimaryDatabaseFromSnapshotFile",
    "Execute",
    "VmSavepoint",
    "VmRollbackToSavepoint",
    "VmReleaseSavepoint",
    "DispatchProcedureException",
    "RunNestedBytecode",
]


def extract_method(text: str, name: str) -> str | None:
    pat = re.compile(rf"^void BytecodeInterpreter::{re.escape(name)}\([^{{]*\)\s*\{{", re.M)
    m = pat.search(text)
    if not m:
        pat = re.compile(rf"^bool BytecodeInterpreter::{re.escape(name)}\([^{{]*\)\s*\{{", re.M)
        m = pat.search(text)
    if not m:
        return None
    start = m.start()
    brace = text.find("{", m.end() - 1)
    depth = 0
    i = brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    return None


def main() -> None:
    text = BC.read_text(encoding="utf-8")
    blocks: list[str] = []
    for name in METHODS:
        block = extract_method(text, name)
        if block:
            blocks.append(block)
            text = text.replace(block + "\n", "", 1)
            text = text.replace(block, "", 1)
    hdr = """\
#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bytecode/QueryCheckpoint.hxx>
#include <SQL/Procedures/BytecodeProcedures.hxx>
#include <IO/Limits.hxx>
#include <IO/Error.hxx>
#include <Database/Database.hxx>
#include <stdexcept>
#include <filesystem>

namespace AstralDB {
namespace SQL {

"""
    OUT.write_text(hdr + "\n\n".join(blocks) + "\n} // namespace SQL\n} // namespace AstralDB\n", encoding="utf-8")
    BC.write_text(text, encoding="utf-8")
    print(f"Moved {len(blocks)} methods to BytecodeVmCore.cxx")


if __name__ == "__main__":
    main()
