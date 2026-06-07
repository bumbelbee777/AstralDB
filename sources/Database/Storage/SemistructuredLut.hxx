#pragma once

#include <DS/FormatDoubleSimd.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace SemistructuredLut {

using StripColumn = FormatDoubleSimd::FormattedDoubleColumn;

[[nodiscard]] std::uint64_t RowSeed(int64_t RowId, std::uint64_t Salt) noexcept;

void AppendStripCell(StripColumn &Col, const char *Data, std::uint16_t Len) noexcept;

void FillPkStrip(const int64_t *RowIds, std::size_t Count, StripColumn &Out) noexcept;

/** Per-row JSON extract strips via \c BulkSyntheticJsonExtractRow (synthetic cell + path parser). */
bool FillJsonExtractStrip(const int64_t *RowIds, std::size_t Count, std::string_view Path, StripColumn &Out) noexcept;

bool FillXmlExtractStrip(const int64_t *RowIds, std::size_t Count, std::string_view Path, StripColumn &Out) noexcept;

bool FillRegexpExtractStrip(const int64_t *RowIds, std::size_t Count, std::string_view Pattern,
                            StripColumn &Out) noexcept;

void FillCharLengthStrip(const int64_t *RowIds, std::size_t Count, StripColumn &Out) noexcept;

void FillRankStripFromRowIndices(const float *Keys, std::size_t KeyCount, const std::uint32_t *RowIndices,
                                 std::size_t Count, StripColumn &Out) noexcept;

using RowItem = std::unordered_map<std::string, std::string>;

void EmplacePkCell(RowItem &Row, const std::string &Col, int64_t RowId) noexcept;
bool EmplaceJsonExtractCell(RowItem &Row, const std::string &Col, int64_t RowId, std::string_view Path) noexcept;
bool EmplaceXmlExtractCell(RowItem &Row, const std::string &Col, int64_t RowId, std::string_view Path) noexcept;
bool EmplaceRegexpExtractCell(RowItem &Row, const std::string &Col, int64_t RowId, std::string_view Pattern) noexcept;
void EmplaceCharLengthCell(RowItem &Row, const std::string &Col, int64_t RowId) noexcept;
void EmplaceRankCell(RowItem &Row, const std::string &Col, float RankKey) noexcept;

} // namespace SemistructuredLut
} // namespace AstralDB
