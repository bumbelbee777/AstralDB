#!/usr/bin/env python3
"""Generate OpcodeMeta table from BytecodeInspect.cxx and BytecodeTypes.hxx."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INSPECT = ROOT / "sources/SQL/Bytecode/BytecodeInspect.cxx"
TYPES = ROOT / "sources/Database/Execution/BytecodeTypes.hxx"
OUT_HXX = ROOT / "sources/SQL/Bytecode/OpcodeMeta.hxx"
OUT_CXX = ROOT / "sources/SQL/Bytecode/OpcodeMeta.cxx"

FLAG_MAP = {
    "OpcodeIsDdl": "kDdl",
    "OpcodeIsDml": "kDml",
    "OpcodeIsJoin": "kJoin",
    "OpcodeIsAggregate": "kAgg",
    "OpcodeIsSecurity": "kSec",
}


def parse_enum(text: str) -> list[str]:
    m = re.search(r"enum class Opcode : uint8_t \{(.*?)\};", text, re.S)
    if not m:
        raise SystemExit("Opcode enum not found")
    body = m.group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    ops: list[str] = []
    for line in body.splitlines():
        line = line.split("//")[0].strip()
        if not line:
            continue
        for part in line.split(","):
            part = part.strip()
            if part and re.match(r"^[A-Z_][A-Z0-9_]*$", part):
                ops.append(part)
    return ops


def parse_names(text: str) -> dict[str, str]:
    names: dict[str, str] = {}
    for m in re.finditer(r"case Opcode::(\w+):\s*\n\s*return \"([^\"]+)\";", text):
        names[m.group(1)] = m.group(2)
    return names


def parse_flags(text: str) -> dict[str, int]:
    flags: dict[str, int] = {}
    flag_bits = {v: 1 << i for i, v in enumerate(FLAG_MAP.values())}
    for fn, bit_name in FLAG_MAP.items():
        m = re.search(rf"bool {fn}\(Opcode Op\) \{{(.*?)\n\}}", text, re.S)
        if not m:
            continue
        for op in re.findall(r"case Opcode::(\w+):", m.group(1)):
            flags[op] = flags.get(op, 0) | flag_bits[bit_name]
    return flags


def main() -> None:
    inspect = INSPECT.read_text(encoding="utf-8")
    types = TYPES.read_text(encoding="utf-8")
    ops = parse_enum(types)
    names = parse_names(inspect)
    flags = parse_flags(inspect)

    OUT_HXX.write_text(
        """#pragma once

#include <Database/Execution/BytecodeTypes.hxx>
#include <cstdint>
#include <string_view>

namespace AstralDB {
namespace SQL {

enum OpcodeMetaFlag : std::uint8_t {
\tkNone = 0,
\tkDdl = 1 << 0,
\tkDml = 1 << 1,
\tkJoin = 1 << 2,
\tkAgg = 1 << 3,
\tkSec = 1 << 4,
};

struct OpcodeMetaEntry {
\tstd::string_view Name;
\tstd::uint8_t Flags;
};

constexpr std::size_t kOpcodeMetaCount = """
        + str(len(ops))
        + """;
extern const OpcodeMetaEntry kOpcodeMeta[kOpcodeMetaCount];

[[nodiscard]] inline const OpcodeMetaEntry &LookupOpcodeMeta(Opcode Op) noexcept {
\tconst auto I = static_cast<std::size_t>(Op);
\tif(I < kOpcodeMetaCount)
\t\treturn kOpcodeMeta[I];
\tstatic constexpr OpcodeMetaEntry kUnknown{\"?\", 0};
\treturn kUnknown;
}

[[nodiscard]] inline std::string_view OpcodeNameView(Opcode Op) noexcept {
\treturn LookupOpcodeMeta(Op).Name;
}

[[nodiscard]] inline bool OpcodeHasFlag(Opcode Op, OpcodeMetaFlag Flag) noexcept {
\treturn (LookupOpcodeMeta(Op).Flags & static_cast<std::uint8_t>(Flag)) != 0;
}

[[nodiscard]] inline bool OpcodeIsDdlMeta(Opcode Op) noexcept {
\treturn OpcodeHasFlag(Op, kDdl);
}
[[nodiscard]] inline bool OpcodeIsDmlMeta(Opcode Op) noexcept {
\treturn OpcodeHasFlag(Op, kDml);
}
[[nodiscard]] inline bool OpcodeIsJoinMeta(Opcode Op) noexcept {
\treturn OpcodeHasFlag(Op, kJoin);
}
[[nodiscard]] inline bool OpcodeIsAggregateMeta(Opcode Op) noexcept {
\treturn OpcodeHasFlag(Op, kAgg);
}
[[nodiscard]] inline bool OpcodeIsSecurityMeta(Opcode Op) noexcept {
\treturn OpcodeHasFlag(Op, kSec);
}

} // namespace SQL
} // namespace AstralDB
""",
        encoding="utf-8",
    )

    lines = ["#include <SQL/Bytecode/OpcodeMeta.hxx>", "", "namespace AstralDB {", "namespace SQL {", ""]
    lines.append("const OpcodeMetaEntry kOpcodeMeta[kOpcodeMetaCount] = {")
    for op in ops:
        nm = names.get(op, op)
        fl = flags.get(op, 0)
        lines.append(f'\t{{"{nm}", {fl}}},')
    lines.append("};")
    lines.extend(["", "} // namespace SQL", "} // namespace AstralDB", ""])
    OUT_CXX.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {len(ops)} opcode entries")


if __name__ == "__main__":
    main()
