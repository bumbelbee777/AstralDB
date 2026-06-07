#pragma once

#include <cstdint>

namespace AstralDB {

enum class ColumnEncoding : uint8_t {
	PlainF64 = 0,
	PlainI64 = 1,
	Dictionary = 2,
	Rle = 3,
	BitPack = 4,
	StringDict = 5,
	LegacyText = 255
};

} // namespace AstralDB
