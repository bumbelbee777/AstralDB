#pragma once

#include <Database/Storage/ColumnarStorage.hxx>

#include <cstdint>
#include <optional>
#include <vector>

namespace AstralDB {

enum class MetadataEligibilityTier : std::uint8_t {
	DensePrecompute = 0,
	ReadOnlyStatInject = 1,
	DemoStatInject = 2,
	PrecomputePending = 3,
	Blocked = 4,
};

enum class BulkDerivationRecipe : std::uint8_t {
	None = 0,
	ClosedFormPass = 1,
	ResidueClassCube = 2,
	TopKFromPassFormula = 3,
	OuterGroupFromSurvivors = 4,
};

[[nodiscard]] bool LegacyPassBitsEnabled() noexcept;

[[nodiscard]] bool MetadataOnlyInsertEnabled() noexcept;

/** When set, metadata-only tables may use closed-form stat injection fast paths (benchmark demos). */
[[nodiscard]] bool MetadataFastPathDemoEnabled() noexcept;

/** True when metadata-only stat injection is allowed without physical pass bits / precompute buffers. */
[[nodiscard]] bool MetadataStatInjectionEligible(const ColumnarTable &Col) noexcept;

[[nodiscard]] MetadataEligibilityTier ClassifyMetadataEligibility(const ColumnarTable &Col,
                                                                  bool ReadOnlyQuery) noexcept;

[[nodiscard]] bool MetadataTierAllowsFastPath(MetadataEligibilityTier Tier) noexcept;

void SetShapeReadOnlyQueryContext(bool ReadOnly) noexcept;

[[nodiscard]] bool ShapeReadOnlyQueryContext() noexcept;

/** Restores read-only shape context on scope exit (metadata matchers set it per query). */
class ShapeReadOnlyQueryContextGuard {
public:
	explicit ShapeReadOnlyQueryContextGuard(bool ReadOnly) { SetShapeReadOnlyQueryContext(ReadOnly); }
	~ShapeReadOnlyQueryContextGuard() { SetShapeReadOnlyQueryContext(false); }
	ShapeReadOnlyQueryContextGuard(const ShapeReadOnlyQueryContextGuard &) = delete;
	ShapeReadOnlyQueryContextGuard &operator=(const ShapeReadOnlyQueryContextGuard &) = delete;
};

void RegisterSyntheticMetadata(ColumnarTable &Col, BulkSyntheticPassFamily Family, std::uint64_t KindMask,
                               int64_t FkModA, int64_t FkModB) noexcept;

/** Residue-class analytic cube (metadata-only JoinFact; O(month segments × cells)). */
void DeriveResidueClassCubeSurvivors(ColumnarTable &Col) noexcept;

[[nodiscard]] std::uint64_t DerivePassCount(const ColumnarTable &Col) noexcept;

/** Row count minus lazy-bulk tombstones (for metadata stat injection). */
[[nodiscard]] std::uint64_t BulkSyntheticLiveRowCount(const ColumnarTable &Col) noexcept;

/** Closed-form row count for \c id range filters on physical-order lazy bulk (\c STEP 1). */
[[nodiscard]] std::optional<std::uint64_t> DeriveLazyBulkIdRangeMatchCount(
    const ColumnarTable &Col, const std::vector<std::vector<FilterPredicateTriple>> &Filters) noexcept;

[[nodiscard]] std::uint32_t DerivePassRatePermille(const ColumnarTable &Col) noexcept;

void EnsureMetadataPassCoverage(ColumnarTable &Col) noexcept;

} // namespace AstralDB
