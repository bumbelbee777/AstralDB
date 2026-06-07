#pragma once

#include <Database/Database.hxx>
#include <Database/Execution/BytecodeTypes.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/ShapeFingerprint.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AstralDB {

enum class BulkQueryKind : std::uint8_t {
	Unknown = 0,
	TableScan,
	FilterLimit,
	WindowLimit,
	CountAgg,
	GroupLimit,
	StarJoinCube,
	StarJoinSelect,
	StarJoinGroup,
	SemistructuredTopk,
	FusedScanFilter,
	MultiJoinLimit,
	RangeWindowLimit,
	GraphTraverse,
	GraphMatch,
	GraphShortestPath,
	GraphPageRank,
	AggregateOnly,
	MultiAggGroupBy,
};

/** Shape summary inferred from bytecode (table scans, filters, window, limit, star bulk ops). */
struct BulkQueryShape {
	BulkQueryKind Kind = BulkQueryKind::Unknown;
	std::string Table;
	std::string FactTable;
	std::string WorkTable;
	std::size_t Limit = 0;
	std::size_t Offset = 0;
	bool HasLimit = false;
	bool HasOffset = false;
	std::size_t WindowCount = 0;
	std::size_t WindowPrecedingRows = 0;
	std::size_t WindowFollowingRows = 0;
	bool WindowFrameRange = false;
	std::string WindowOutCol;
	std::vector<std::vector<FilterPredicateTriple>> Filters;
	bool HasCountAgg = false;
	bool HasGroupBy = false;
	std::int64_t HavingMin = 0;
	std::size_t InnerJoins = 0;
	bool OrderDescending = false;
	std::vector<std::string> Projections;
	std::vector<std::pair<std::string, bool>> OrderByKeys;
	std::vector<std::string> GroupByKeys;
	PredicateKindMask QueryFilterMask = 0;
	bool ReadOnly = false;
	std::uint8_t AggFnMask = 0;
	std::uint8_t GraphOpcodeTag = 0;
};

[[nodiscard]] bool InferBulkQueryShapeFromBytecode(const SQL::Bytecode &Code, BulkQueryShape &Out) noexcept;

[[nodiscard]] bool BytecodeIsReadOnlyQuery(const SQL::Bytecode &Code) noexcept;

[[nodiscard]] QueryShapeFingerprint128 QueryShapeFingerprint128FromBytecode(const SQL::Bytecode &Code,
                                                                            const BulkQueryShape &Shape) noexcept;

[[nodiscard]] std::string ResolveBulkQuerySourceTable(const SQL::Bytecode &Code, std::string_view Table) noexcept;

void ApplyBulkQueryShapeHints(ColumnarTable &Col, const BulkQueryShape &Shape,
                              const QueryShapeFingerprint128 &Fingerprint) noexcept;

[[nodiscard]] MetadataFastPathHit MatchBulkQueryMetadata(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                                         const std::vector<Database::Column> *Schema,
                                                         bool ReadOnlyQuery) noexcept;

void RegisterUniversalLazyBulkPrecomputeManifest(ColumnarTable &Col,
                                                 const std::vector<Database::Column> &Schema) noexcept;

[[nodiscard]] std::uint32_t BuildBulkSchemaKindMask(const std::vector<Database::Column> &Schema) noexcept;

[[nodiscard]] const char *BulkQueryKindName(BulkQueryKind Kind) noexcept;

/** Closed-form filtered row estimate for shape metadata and composition trees. */
[[nodiscard]] std::uint64_t DeriveBulkShapeFilteredRowCount(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                                            const std::vector<Database::Column> *Schema) noexcept;

[[nodiscard]] MetadataFastPathHit MatchBulkQueryMetadataWithComposition(const ColumnarTable &Col,
                                                                        const BulkQueryShape &Shape,
                                                                        const SQL::Bytecode &Code,
                                                                        const std::vector<Database::Column> *Schema,
                                                                        bool ReadOnlyQuery) noexcept;

} // namespace AstralDB
