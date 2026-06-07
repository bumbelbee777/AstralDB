#pragma once

#include <Database/Storage/BulkQueryMetadata.hxx>
#include <Database/Storage/BulkSyntheticDerive.hxx>
#include <Database/Storage/ShapeFingerprint.hxx>

#include <cstdint>

namespace AstralDB {
namespace SQL {

enum class ShapeRouteTier : std::uint8_t {
	MetadataO1 = 0,
	FusedDominant = 1,
	JitAccelerated = 2,
	FullVm = 3,
};

struct ShapeTelemetry {
	ShapeRouteTier RouteTier = ShapeRouteTier::FullVm;
	MetadataEligibilityTier EligibilityTier = MetadataEligibilityTier::Blocked;
	QueryShapeFingerprint128 Fingerprint{};
	BulkQueryKind ShapeKind = BulkQueryKind::Unknown;
	std::uint8_t MatcherMiss = 0;
	bool MetadataHit = false;
	std::uint32_t ObservedLimitK = 0;
	std::uint16_t CompositionNodes = 0;
};

[[nodiscard]] const char *ShapeRouteTierName(ShapeRouteTier Tier) noexcept;
[[nodiscard]] const char *MetadataEligibilityTierName(MetadataEligibilityTier Tier) noexcept;

} // namespace SQL
} // namespace AstralDB
