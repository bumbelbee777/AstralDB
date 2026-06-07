#pragma once

#include <Database/Database.hxx>

struct ColumnarTable;
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** Schema-driven synthetic cell generation for \c INSERT … BULK (no column-name tables). */
struct BulkSyntheticContext {
	int64_t RowId = 0;
	int64_t Step = 1;
	std::size_t ColIndex = 0;
	std::size_t ColCount = 0;
};

enum class BulkSyntheticValueKind : uint8_t {
	PrimaryKey,
	ForeignKey,
	Integer,
	Decimal,
	Timestamp,
	Text,
	Advanced
};

BulkSyntheticValueKind ClassifyBulkColumn(const Database::Column &Col, std::size_t ColIndex, std::size_t ColCount);

std::string BulkSyntheticCellString(const Database::Column &Col, const BulkSyntheticContext &Ctx);

std::string BulkSyntheticCountry(int64_t RowId);
std::string BulkSyntheticCategory(int64_t RowId);

inline constexpr std::uint8_t BulkSyntheticCountryCount = 8;
inline constexpr std::uint8_t BulkSyntheticCategoryCount = 7;
inline constexpr std::size_t BulkSyntheticWarehouseLutSlots =
    static_cast<std::size_t>(BulkSyntheticCountryCount) * static_cast<std::size_t>(BulkSyntheticCategoryCount);

/** Branchless RowSeed-derived group key indices (no string materialization). */
[[nodiscard]] std::uint8_t BulkSyntheticCountryIndex(int64_t RowId) noexcept;
[[nodiscard]] std::uint8_t BulkSyntheticCategoryIndex(int64_t RowId) noexcept;

/** Warehouse cube slot using insert-time FK LUTs when present (O(1) vs re-hash). */
[[nodiscard]] inline std::uint8_t BulkSyntheticWarehouseSlotFromCol(const ColumnarTable &Col, int64_t LinkedRowId,
                                                                    int64_t SecondDimRowId) noexcept {
	std::uint8_t Ci = 0;
	if(!Col.BulkSyntheticCountryLut.empty() && LinkedRowId >= 1 &&
	   static_cast<std::size_t>(LinkedRowId) <= Col.BulkSyntheticCountryLut.size())
		Ci = Col.BulkSyntheticCountryLut[static_cast<std::size_t>(LinkedRowId - 1)];
	else
		Ci = BulkSyntheticCountryIndex(LinkedRowId);
	std::uint8_t Gi = 0;
	if(!Col.BulkSyntheticCategoryLut.empty() && SecondDimRowId >= 1 &&
	   static_cast<std::size_t>(SecondDimRowId) <= Col.BulkSyntheticCategoryLut.size())
		Gi = Col.BulkSyntheticCategoryLut[static_cast<std::size_t>(SecondDimRowId - 1)];
	else
		Gi = BulkSyntheticCategoryIndex(SecondDimRowId);
	return static_cast<std::uint8_t>(Ci * BulkSyntheticCategoryCount + Gi);
}
[[nodiscard]] std::string_view BulkSyntheticCountryNameByIndex(std::uint8_t Index) noexcept;
[[nodiscard]] std::string_view BulkSyntheticCategoryNameByIndex(std::uint8_t Index) noexcept;

/** Optional scalar fast path for synthetic rows (disabled; full evaluation used). */
bool BulkSyntheticTryScalarEval(int FnTag, int64_t RowId, const std::vector<std::pair<int64_t, std::string>> &Args,
                                std::string &Out);

/** Predicate fast path for lazy bulk WHERE. Nullopt = not handled. */
std::optional<bool> BulkSyntheticTryMatchPredicate(int64_t RowId, const std::string &Col, const std::string &Op,
                                                   const std::string &Rhs, const Database::Column *ColDef);

/** True when every lazy-bulk row satisfies \a Op \a Bound on monotonic synthetic timestamps. */
[[nodiscard]] bool BulkSyntheticTimestampLowerBoundMatchesAllBulkRows(const ColumnarTable &Col, std::string_view Op,
                                                                     std::string_view Bound) noexcept;

double BulkSyntheticDecimalFromRowId(int64_t RowId) noexcept;

/** \c Out[i] = BulkSyntheticDecimalFromRowId(StartId + i*Step) for \c i in [0,Count). Step==1 uses incremental state (no per-row div). */
void BulkSyntheticFillDecimalByRowRange(int64_t StartId, int64_t Step, std::size_t Count, double *Out) noexcept;

/** Sum of \c BulkSyntheticDecimalFromRowId(StartRowId + k*Step) for \c k in [0, Count). */
[[nodiscard]] double BulkSyntheticSumDecimalByRowProgression(int64_t StartRowId, int64_t Step,
                                                             std::size_t Count) noexcept;

/** PARTITION BY ((RowId-1)%Mod)+1 with contiguous RowIds and Step==1 yields one row per partition. */
[[nodiscard]] bool BulkSyntheticFkPartitionsSingletonPerRow(int64_t Step, int64_t PartitionMod) noexcept;

/** ISO-8601 UTC timestamp from Unix epoch seconds. */
std::string BulkSyntheticIsoTimestamp(int64_t EpochSec);

/** Canonical synthetic JSON/XML/bio payloads for lazy-bulk row-id evaluation. */
[[nodiscard]] std::string BulkSyntheticJsonCell(int64_t RowId, uint64_t Salt = 0);
[[nodiscard]] std::string BulkSyntheticXmlCell(int64_t RowId);
[[nodiscard]] std::string BulkSyntheticBioText(int64_t RowId);

/** Modulus for FK-style ints derived from schema (stable across columns). */
int64_t BulkSyntheticFkModulus(const Database::Column &Col) noexcept;

[[nodiscard]] int64_t BulkSyntheticRowIdAt(const ColumnarTable &Col, std::size_t RowIndex) noexcept;

/** Month bucket for synthetic time OLAP keys (epoch derived from bulk row id). */
[[nodiscard]] int64_t BulkSyntheticMonthBucketFromRowId(int64_t RowId) noexcept;

/** Numeric join/filter key without allocating a string (PK / FK / integer columns). */
[[nodiscard]] bool BulkSyntheticTryInt64Key(const Database::Column &Col, int64_t RowId, int64_t &Out) noexcept;

/** Wide JSON/XML/vector columns skipped on slim join/scan paths. */
[[nodiscard]] bool BulkSyntheticIsHeavyColumn(const Database::Column &Col) noexcept;

/** Schema column index for \a ColName, or \c Schema.size() if missing. */
[[nodiscard]] std::size_t BulkSyntheticColumnIndex(const std::vector<Database::Column> &Schema,
                                                   std::string_view ColName) noexcept;

} // namespace AstralDB
