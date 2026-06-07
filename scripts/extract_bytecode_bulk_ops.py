#!/usr/bin/env python3
"""Extract bulk opcode case arms from BytecodeInterpreter::Step into StepBulk."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BC = ROOT / "sources/SQL/Bytecode/Bytecode.cxx"
OUT = ROOT / "sources/SQL/Bytecode/BytecodeBulkOps.cxx"
HXX = ROOT / "sources/SQL/Bytecode/BytecodeInterpreter.hxx"

BULK_OPCODES = {
    "INSERT_BULK",
    "COUNT_BULK",
    "JOIN_COUNT_BULK",
    "FILTER_COUNT_BULK",
    "GROUP_BY_BULK",
    "STAR_GROUP_BY_BULK",
    "STAR_JOIN_CUBE_BULK",
    "STAR_JOIN_SELECT_BULK",
    "STAR_JOIN_GROUP_BULK",
    "SEMISTRUCTURED_TOPK_BULK",
    "FUSED_SEMISTRUCTURED_SCAN",
    "FUSED_SEMI_JOIN_EXISTS",
    "FUSED_SCAN_FILTER",
    "FUSED_SCAN_FILTER_AGG",
    "FUSED_SCAN_PROJECT_LIMIT",
    "FUSED_JOIN_FILTER",
    "FUSED_PRECOMPUTED_AGG",
}


def extract_case_block(text: str, opcode: str) -> str | None:
    pat = re.compile(rf"case Opcode::{opcode}:\s*\{{", re.M)
    m = pat.search(text)
    if not m:
        pat = re.compile(rf"case Opcode::{opcode}:", re.M)
        m = pat.search(text)
        if not m:
            return None
        if text[m.end() : m.end() + 1] != "{":
            end = text.find("break;", m.end())
            if end == -1:
                return None
            return text[m.start() : end + 6]
    start = m.start()
    brace = text.find("{", m.start())
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
    for op in BULK_OPCODES:
        block = extract_case_block(text, op)
        if block:
            blocks.append(block)
            text = text.replace(block + "\n", "", 1)
            text = text.replace(block, "", 1)

    if not blocks:
        raise SystemExit("No bulk opcode cases extracted")

    hdr = """\
#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Bulk/BulkOps.hxx>
#include <SQL/Bulk/BulkDominantAmb.hxx>
#include <SQL/Bulk/BulkDominantWarehouseMegafusion.hxx>
#include <SQL/Bulk/SqlBytecodeTail.hxx>
#include <SQL/Bytecode/FastPathGuard.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <IO/Limits.hxx>
#include <IO/Error.hxx>

namespace AstralDB {
namespace SQL {

namespace {
[[noreturn]] inline void FailVm(std::string Message) {
	throw std::runtime_error(AstralDB::Err::Prefixed("SQL VM", std::move(Message)));
}
} // namespace

bool BytecodeInterpreter::StepBulk(const Bytecode &Code, const Instruction &Inst) {
\t(void)Code;
\tswitch(Inst.Opcode_) {
"""
    body = "\n".join("        " + b.replace("\n", "\n        ") for b in blocks)
    ftr = """
        default:
            return false;
    }
    return true;
}

} // namespace SQL
} // namespace AstralDB
"""
    OUT.write_text(hdr + body + ftr, encoding="utf-8")

    hook = """
        if(StepBulk(Code, inst))
            break;
"""
    step_pat = re.compile(r"(bool BytecodeInterpreter::Step\(const Bytecode &Code\) \{.*?switch\(inst\.Opcode_\) \{)", re.S)
    m = step_pat.search(text)
    if not m:
        raise SystemExit("Step() switch not found")
    insert_at = m.end()
    text = text[:insert_at] + hook + text[insert_at:]
    BC.write_text(text, encoding="utf-8")

    if "StepBulk" not in HXX.read_text(encoding="utf-8"):
        HXX.write_text(
            HXX.read_text(encoding="utf-8").replace(
                "    bool Step(const Bytecode &Code);",
                "    bool Step(const Bytecode &Code);\n    bool StepBulk(const Bytecode &Code, const Instruction &Inst);",
            ),
            encoding="utf-8",
        )
    print(f"Extracted {len(blocks)} bulk opcode cases")


if __name__ == "__main__":
    main()
