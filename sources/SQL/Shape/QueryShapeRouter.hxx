#pragma once

#include <Database/Storage/BulkQueryMetadata.hxx>
#include <Database/Storage/BulkSyntheticDerive.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Optimizer/ConfidenceScoring.hxx>
#include <SQL/Shape/ShapeCompositionTypes.hxx>
#include <SQL/Shape/ShapeTelemetryTypes.hxx>

#include <sstream>

namespace AstralDB {
namespace SQL {

struct ShapeRouteDecision {
	ShapeRouteTier RouteTier = ShapeRouteTier::FullVm;
	MetadataEligibilityTier EligibilityTier = MetadataEligibilityTier::Blocked;
	BulkQueryShape Shape{};
	QueryShapeComposition Composition{};
	bool HasComposition = false;
	QueryShapeFingerprint128 Fingerprint{};
	Confidence OptConfidence = Confidence::Reject;
	PassBitEligibilityMiss MatcherMiss = PassBitEligibilityMiss::None;
};

[[nodiscard]] ShapeRouteDecision ClassifyQueryShape(BytecodeInterpreter &Vm, const Bytecode &Code) noexcept;

[[nodiscard]] bool TryExecuteShapeRoute(BytecodeInterpreter &Vm, const Bytecode &Code,
                                        const ShapeRouteDecision &Decision) noexcept;

void RecordShapeTelemetry(BytecodeInterpreter &Vm, const Bytecode &Code, const ShapeRouteDecision &Decision,
                          bool MetadataHit) noexcept;

[[nodiscard]] bool TryExecuteReadOnlyViaShapeRouter(BytecodeInterpreter &Vm, const Bytecode &Code) noexcept;

[[nodiscard]] bool ShapeTelemetryEnabled() noexcept;

void AppendShapeTelemetryFields(std::ostringstream &O, const BytecodeInterpreter::TimeSqlStats &Stats) noexcept;

} // namespace SQL
} // namespace AstralDB
