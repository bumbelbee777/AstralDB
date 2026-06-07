#pragma once

#include <DS/FormatDoubleSimd.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** SplitMix64 row hash (matches lazy-bulk synthetic payloads). */
[[nodiscard]] std::uint64_t BulkSyntheticRowSeed(int64_t RowId, std::uint64_t Salt) noexcept;

enum class BulkSyntheticJsonLeaf : std::uint8_t {
	Unknown = 0,
	ActiveBool,
	PaymentMethod,
	PreferencesTheme,
	PreferencesNotificationsEmail,
};

struct BulkSyntheticJsonPathPlan {
	BulkSyntheticJsonLeaf Leaf = BulkSyntheticJsonLeaf::Unknown;
};

/** Map dot-path (with or without \c $. prefix) to a generator-schema leaf; unknown paths use JSON parse fallback. */
[[nodiscard]] BulkSyntheticJsonPathPlan PlanBulkSyntheticJsonPath(std::string_view Path) noexcept;

enum class BulkSyntheticXmlLeaf : std::uint8_t {
	Unknown = 0,
	SettingsLanguage,
};

struct BulkSyntheticXmlPathPlan {
	BulkSyntheticXmlLeaf Leaf = BulkSyntheticXmlLeaf::Unknown;
};

[[nodiscard]] BulkSyntheticXmlPathPlan PlanBulkSyntheticXmlPath(std::string_view Path) noexcept;

bool BulkSyntheticJsonExtractPlanned(int64_t RowId, const BulkSyntheticJsonPathPlan &Plan, std::string_view Path,
                                     std::string &Out) noexcept;

bool BulkSyntheticXmlExtractPlanned(int64_t RowId, const BulkSyntheticXmlPathPlan &Plan, std::string_view Path,
                                    std::string &Out) noexcept;

void BulkSyntheticJsonFillStrip(const int64_t *RowIds, std::size_t Count, std::string_view Path,
                                FormatDoubleSimd::FormattedDoubleColumn &Out) noexcept;

void BulkSyntheticXmlFillStrip(const int64_t *RowIds, std::size_t Count, std::string_view Path,
                               FormatDoubleSimd::FormattedDoubleColumn &Out) noexcept;

void BulkSyntheticRegexpFillStrip(const int64_t *RowIds, std::size_t Count, std::string_view Pattern,
                                  FormatDoubleSimd::FormattedDoubleColumn &Out) noexcept;

} // namespace AstralDB
