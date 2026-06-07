#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {

inline constexpr std::size_t kVectorizedScanBatch = 1024;

struct ScanBatchI64 {
	std::size_t BeginRow = 0;
	std::size_t Count = 0;
	std::vector<int64_t> Values;
	std::vector<std::size_t> RowIndices;
};

/** Fill batch with parsed int64 column values and original row indices. */
void FillScanBatchI64(const std::vector<std::string> &Column, std::size_t Begin, std::size_t End,
                      ScanBatchI64 &Out);

} // namespace AstralDB
