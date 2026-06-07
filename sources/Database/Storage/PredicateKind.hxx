#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnarStorage.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** Schema- and operator-driven predicate kinds (no table/column name tables). */
enum class PredicateKindFlag : std::uint64_t {
	None = 0,
	JsonExtractEq = 1ULL << 0,
	JsonExtractNe = 1ULL << 1,
	XmlValid = 1ULL << 10,
	TextMatch = 1ULL << 15,
	RegexMatch = 1ULL << 17,
	ColumnGe = 1ULL << 23,
	ColumnLe = 1ULL << 24,
	TimestampGe = 1ULL << 30,
	TimestampLe = 1ULL << 31,
	/** Synthetic POINT cells always fall inside global WGS84 bbox. */
	StWithinBbox = 1ULL << 32,
	/** JSON \c __IN__ on planned metadata paths (e.g. payment_method). */
	JsonExtractIn = 1ULL << 33,
};

using PredicateKindMask = std::uint64_t;

[[nodiscard]] PredicateKindMask PredicateKindBit(PredicateKindFlag Flag) noexcept;

/** Declared SQL storage class from \a Col.DefaultValue (not column name). */
enum class SqlStorageKind : std::uint8_t {
	Unknown,
	Json,
	Xml,
	Text,
	Timestamp,
	Decimal,
	Integer,
	ForeignKey,
	Other,
};

[[nodiscard]] SqlStorageKind ClassifySqlStorage(const Database::Column &Col) noexcept;

[[nodiscard]] const Database::Column *FindSchemaColumn(const std::vector<Database::Column> &Schema,
                                                       std::string_view Name) noexcept;

[[nodiscard]] PredicateKindMask PredicateKindsForColumn(const Database::Column &Col) noexcept;

[[nodiscard]] PredicateKindMask DetectPredicateKindsFromSchema(
    const std::vector<Database::Column> &Schema) noexcept;

[[nodiscard]] PredicateKindMask DetectPredicateKindForTriple(const std::string &ColName, const std::string &Op,
                                                             const std::string &Rhs,
                                                             const Database::Column *ColDef,
                                                             bool ColumnHasFtsIndex) noexcept;

struct QueryMaskBuildResult {
	PredicateKindMask Mask = 0;
	/** True when every conjunct in the sole DNF branch maps to a predicate kind. */
	bool AllPredicatesRecognized = false;
};

[[nodiscard]] QueryMaskBuildResult BuildQueryMaskFromDnfEx(const BulkWhereDnf &Dnf,
                                                           const std::vector<Database::Column> &PrimarySchema,
                                                           const std::vector<Database::Column> *LinkedSchema,
                                                           const Database *Db,
                                                           const std::string &PrimaryTable) noexcept;

[[nodiscard]] PredicateKindMask BuildQueryMaskFromDnf(const BulkWhereDnf &Dnf,
                                                      const std::vector<Database::Column> &PrimarySchema,
                                                      const std::vector<Database::Column> *LinkedSchema,
                                                      const Database *Db, const std::string &PrimaryTable) noexcept;

[[nodiscard]] PredicateKindMask PassKindsEvaluatedForFamily(BulkSyntheticPassFamily Family) noexcept;

[[nodiscard]] bool SyntheticRowPassesPredicateMask(BulkSyntheticPassFamily Family, int64_t PrimaryRowId,
                                                   int64_t LinkedRowId, PredicateKindMask KindMask,
                                                   bool PrimaryHasJson, bool LinkedHasJson,
                                                   bool PrimaryHasReviewText) noexcept;

[[nodiscard]] bool PassBitsCoverQuery(const ColumnarTable &Col, PredicateKindMask QueryMask) noexcept;

[[nodiscard]] bool PassBitsApplicable(const BulkWhereDnf *Filters, const ColumnarTable &Col,
                                      const std::vector<Database::Column> &PrimarySchema,
                                      const std::vector<Database::Column> *LinkedSchema,
                                      PredicateKindMask QueryMask) noexcept;

} // namespace AstralDB
