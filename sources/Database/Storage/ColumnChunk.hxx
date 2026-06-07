#pragma once

#include <Database/Storage/ColumnEncoding.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {

/** In-memory or on-disk column chunk metadata. */
struct ColumnChunk {
	ColumnEncoding Encoding = ColumnEncoding::LegacyText;
	std::size_t RowCount = 0;
	std::size_t NullBitmapOffset = 0;
	std::size_t DataOffset = 0;
	std::size_t CompressedSize = 0;
	double MinNumeric = 0.0;
	double MaxNumeric = 0.0;
	std::size_t Cardinality = 0;
	std::vector<std::byte> Payload;
};

} // namespace AstralDB
