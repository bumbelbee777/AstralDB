#pragma once

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Database.hxx>

#include <cstdint>
#include <vector>

namespace AstralDB {

/** Composable synthetic predicate kinds (bit flags, not query-specific). */
enum class BulkSyntheticPredKind : std::uint64_t {
	None = 0,
	TimestampLowerBound = 1ULL << 0,
	JsonExtractEq = 1ULL << 1,
	XmlValid = 1ULL << 2,
	ReviewSentiment = 1ULL << 3,
	BioSqlKeyword = 1ULL << 4,
};

[[nodiscard]] std::uint64_t BulkSyntheticPredKindBit(BulkSyntheticPredKind Kind) noexcept;

/** Predicate kinds actually evaluated when building pass bits for \a Family. */
[[nodiscard]] std::uint64_t BulkSyntheticPassKindsEvaluated(BulkSyntheticPassFamily Family) noexcept;

/** Bitmask of predicate kinds composable on columns present in \a Schema. */
[[nodiscard]] std::uint64_t BulkSyntheticDetectPassKindMaskFromSchema(
    const std::vector<Database::Column> &Schema) noexcept;

/** Kind mask for a workload family (warehouse merges primary + linked schema). */
[[nodiscard]] std::uint64_t BulkSyntheticDetectPassKindMask(
    BulkSyntheticPassFamily Family, const std::vector<Database::Column> &PrimarySchema,
    const std::vector<Database::Column> *LinkedSchema = nullptr) noexcept;

[[nodiscard]] std::uint64_t BulkSyntheticClassifyPredicateKind(const std::string &Col, const std::string &Op,
                                                               const std::string &Rhs,
                                                               const Database::Column *ColDef);

[[nodiscard]] std::uint64_t BulkSyntheticDnfRequiredKindMask(
    const BulkWhereDnf &Dnf, const std::vector<Database::Column> &PrimarySchema,
    const std::vector<Database::Column> *LinkedSchema = nullptr);

[[nodiscard]] bool BulkSyntheticRowPassesKindMask(BulkSyntheticPassFamily Family, int64_t PrimaryRowId,
                                                  int64_t LinkedRowId, std::uint64_t KindMask) noexcept;

void BuildBulkSyntheticPassBits(ColumnarTable &Col, BulkSyntheticPassFamily Family, std::uint64_t KindMask,
                                int64_t FkCustMod = 0, int64_t FkProdMod = 0);

/** Insert-time FK dimension LUTs (country/category by linked row id). */
void EnsureJoinFactFkLuts(ColumnarTable &Col, int64_t FkModA, int64_t FkModB) noexcept;

/** Build \c BulkSyntheticPassSparseWords from pass bits (no-op if already built). */
void BuildBulkSyntheticPassSparseWords(ColumnarTable &Col) noexcept;

void EnsureBulkSyntheticPassSparseWords(ColumnarTable &Col) noexcept;

[[nodiscard]] bool BulkSyntheticPassBitsCoverQuery(const ColumnarTable &Col, std::uint64_t QueryKindMask) noexcept;

[[nodiscard]] std::uint64_t BulkSyntheticCountPassBits(const ColumnarTable &Col) noexcept;

[[nodiscard]] bool BulkSyntheticJoinFactReady(const ColumnarTable &Col) noexcept;

[[nodiscard]] bool BulkSyntheticFusedJoinAggReady(const ColumnarTable &Col) noexcept;

[[nodiscard]] bool BulkSyntheticPassBitAt(const ColumnarTable &Col, std::size_t RowIndex) noexcept;

[[nodiscard]] bool BulkSyntheticPassBitsApplicable(const BulkWhereDnf *Filters, const ColumnarTable &Col,
                                                   const std::vector<Database::Column> &PrimarySchema,
                                                   const std::vector<Database::Column> *LinkedSchema,
                                                   std::uint64_t QueryKindMask) noexcept;

} // namespace AstralDB
