#pragma once

#include <Database/Storage/BulkQueryMetadata.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/ShapeFingerprint.hxx>

#include <array>
#include <cstdint>

namespace AstralDB {

struct ColumnarTable;

namespace SQL {

/** Relational-algebra primitives composable in O(1) (bounded node cap). */
enum class ElementaryShapeKind : std::uint8_t {
	None = 0,
	Scan,
	Filter,
	Project,
	InnerJoin,
	GroupBy,
	Having,
	OrderBy,
	Limit,
	Offset,
	Window,
	Distinct,
	UnionAll,
	UnionDistinct,
	Intersect,
	Except,
	SubqueryScan,
	CteScan,
	AggregateScalar,
	ExistsSemi,
	FusedScanFilter,
	StarJoin,
	GraphOp,
};

struct ShapeCompositionNode {
	ElementaryShapeKind Kind = ElementaryShapeKind::None;
	BulkQueryShape Payload{};
	std::uint16_t ChildA = UINT16_MAX;
	std::uint16_t ChildB = UINT16_MAX;
};

struct QueryShapeComposition {
	static constexpr std::size_t MaxNodes = 32;
	std::array<ShapeCompositionNode, MaxNodes> Nodes{};
	std::size_t Count = 0;
	std::uint16_t Root = UINT16_MAX;
};

[[nodiscard]] MetadataFastPathHit MatchCompositionMetadata(const ColumnarTable &Col, const QueryShapeComposition &Comp,
                                                           const std::vector<Database::Column> *Schema,
                                                           bool ReadOnlyQuery) noexcept;

[[nodiscard]] QueryShapeFingerprint128 QueryShapeFingerprint128FromComposition(
    const QueryShapeComposition &Comp) noexcept;

[[nodiscard]] const char *ElementaryShapeKindName(ElementaryShapeKind Kind) noexcept;

[[nodiscard]] BulkQueryKind BulkQueryKindFromCompositionRoot(const QueryShapeComposition &Comp) noexcept;

} // namespace SQL
} // namespace AstralDB
