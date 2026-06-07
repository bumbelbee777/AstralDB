#pragma once

#include <Database/Execution/BytecodeTypes.hxx>
#include <cstdint>
#include <string_view>

namespace AstralDB {
namespace SQL {

enum OpcodeMetaFlag : std::uint8_t {
	kNone = 0,
	kDdl = 1 << 0,
	kDml = 1 << 1,
	kJoin = 1 << 2,
	kAgg = 1 << 3,
	kSec = 1 << 4,
};

struct OpcodeMetaEntry {
	std::string_view Name;
	std::uint8_t Flags;
};

constexpr std::size_t kOpcodeMetaCount = 176;
extern const OpcodeMetaEntry kOpcodeMeta[kOpcodeMetaCount];

[[nodiscard]] inline const OpcodeMetaEntry &LookupOpcodeMeta(Opcode Op) noexcept {
	const auto I = static_cast<std::size_t>(Op);
	if(I < kOpcodeMetaCount)
		return kOpcodeMeta[I];
	static constexpr OpcodeMetaEntry kUnknown{"?", 0};
	return kUnknown;
}

[[nodiscard]] inline std::string_view OpcodeNameView(Opcode Op) noexcept {
	return LookupOpcodeMeta(Op).Name;
}

[[nodiscard]] inline bool OpcodeHasFlag(Opcode Op, OpcodeMetaFlag Flag) noexcept {
	return (LookupOpcodeMeta(Op).Flags & static_cast<std::uint8_t>(Flag)) != 0;
}

[[nodiscard]] inline bool OpcodeIsDdlMeta(Opcode Op) noexcept {
	return OpcodeHasFlag(Op, kDdl);
}
[[nodiscard]] inline bool OpcodeIsDmlMeta(Opcode Op) noexcept {
	return OpcodeHasFlag(Op, kDml);
}
[[nodiscard]] inline bool OpcodeIsJoinMeta(Opcode Op) noexcept {
	return OpcodeHasFlag(Op, kJoin);
}
[[nodiscard]] inline bool OpcodeIsAggregateMeta(Opcode Op) noexcept {
	return OpcodeHasFlag(Op, kAgg);
}
[[nodiscard]] inline bool OpcodeIsSecurityMeta(Opcode Op) noexcept {
	return OpcodeHasFlag(Op, kSec);
}

} // namespace SQL
} // namespace AstralDB
