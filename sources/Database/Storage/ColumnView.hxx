#pragma once

#include <Database/Storage/ColumnChunk.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** Zero-copy slice over mmap shard or in-memory chunk. */
class ColumnView {
public:
	ColumnView() = default;
	ColumnView(const std::byte *Data, std::size_t Size, ColumnEncoding Enc, std::size_t RowCount);

	[[nodiscard]] ColumnEncoding Encoding() const noexcept { return Encoding_; }
	[[nodiscard]] std::size_t RowCount() const noexcept { return RowCount_; }
	[[nodiscard]] bool IsNull(std::size_t Row) const;
	[[nodiscard]] double AsF64(std::size_t Row) const;
	[[nodiscard]] int64_t AsI64(std::size_t Row) const;
	[[nodiscard]] std::string AsString(std::size_t Row) const;

	void DecodeF64Lane(std::size_t Begin, std::size_t Count, double *Out) const;
	void GatherStrings(std::size_t Begin, std::size_t Count, std::vector<std::string> &Out) const;

private:
	const std::byte *Data_ = nullptr;
	std::size_t Size_ = 0;
	ColumnEncoding Encoding_ = ColumnEncoding::LegacyText;
	std::size_t RowCount_ = 0;
	std::size_t NullBitmapOffset_ = 0;
};

/** Column batch handle for late-materialization pipelines. */
struct ColumnBatch {
	std::size_t BeginRow = 0;
	std::size_t EndRow = 0;
	std::vector<ColumnView> Columns;
	std::vector<std::string> ColumnNames;
};

} // namespace AstralDB
