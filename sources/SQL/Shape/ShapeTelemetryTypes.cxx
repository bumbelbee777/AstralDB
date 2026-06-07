#include <SQL/Shape/ShapeTelemetryTypes.hxx>

namespace AstralDB {
namespace SQL {

const char *ShapeRouteTierName(const ShapeRouteTier Tier) noexcept {
	switch(Tier) {
	case ShapeRouteTier::MetadataO1:
		return "metadata_o1";
	case ShapeRouteTier::FusedDominant:
		return "fused";
	case ShapeRouteTier::JitAccelerated:
		return "jit";
	case ShapeRouteTier::FullVm:
		return "vm";
	}
	return "vm";
}

const char *MetadataEligibilityTierName(const MetadataEligibilityTier Tier) noexcept {
	switch(Tier) {
	case MetadataEligibilityTier::DensePrecompute:
		return "dense";
	case MetadataEligibilityTier::ReadOnlyStatInject:
		return "readonly";
	case MetadataEligibilityTier::DemoStatInject:
		return "demo";
	case MetadataEligibilityTier::PrecomputePending:
		return "pending";
	case MetadataEligibilityTier::Blocked:
		return "blocked";
	}
	return "blocked";
}

} // namespace SQL
} // namespace AstralDB
