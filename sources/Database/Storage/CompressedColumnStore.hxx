#pragma once

#include <Database/Storage/ColumnChunk.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

/** Typed compressed column-major table storage. */
class CompressedColumnStore {
public:
	struct ColumnCatalog {
		std::vector<ColumnChunk> Chunks;
		std::size_t TotalRows = 0;
	};

	void Clear();
	void AppendChunk(const std::string &Column, ColumnChunk Chunk);
	[[nodiscard]] const ColumnCatalog *FindColumn(const std::string &Column) const;
	[[nodiscard]] std::size_t RowCount() const noexcept { return RowCount_; }
	[[nodiscard]] const std::unordered_map<std::string, ColumnCatalog> &Columns() const noexcept { return Columns_; }

	void AppendPlainF64Chunk(const std::string &Column, const double *Values, std::size_t Count,
	                         const std::vector<bool> &Nulls = {});
	void AppendPlainI64Chunk(const std::string &Column, const int64_t *Values, std::size_t Count,
	                         const std::vector<bool> &Nulls = {});

private:
	std::unordered_map<std::string, ColumnCatalog> Columns_;
	std::size_t RowCount_ = 0;
};

} // namespace AstralDB
