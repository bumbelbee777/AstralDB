#include <SQL/Shape/QueryShapeRouter.hxx>

#include <Database/Storage/ShapeWorkloadRegistry.hxx>
#include <SQL/Shape/ShapeComposition.hxx>
#include <SQL/Bulk/BulkDominantAmb.hxx>
#include <SQL/Bulk/BulkDominantWarehouseMegafusion.hxx>
#include <SQL/Bulk/BulkOps.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Optimizer/ConfidenceScoring.hxx>

#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace AstralDB {
namespace SQL {

namespace {

[[nodiscard]] bool EnvTruthy(const char *Name) noexcept {
	const char *E = std::getenv(Name);
	return E != nullptr && E[0] != '0' && E[0] != '\0';
}

[[nodiscard]] bool HasFusedDominantOpcodes(const Bytecode &Code) noexcept {
	for(const Instruction &Inst : Code) {
		switch(Inst.Opcode_) {
		case Opcode::STAR_JOIN_CUBE_BULK:
		case Opcode::STAR_JOIN_SELECT_BULK:
		case Opcode::STAR_JOIN_GROUP_BULK:
		case Opcode::SEMISTRUCTURED_TOPK_BULK:
		case Opcode::FUSED_SEMISTRUCTURED_SCAN:
		case Opcode::FUSED_SCAN_FILTER:
		case Opcode::FUSED_SCAN_FILTER_AGG:
			return true;
		default:
			break;
		}
	}
	return false;
}

} // namespace

bool ShapeTelemetryEnabled() noexcept {
	if(EnvTruthy("ASTRALDB_SHAPE_TELEMETRY"))
		return true;
	return EnvTruthy("ASTRALDB_TIME_SQL");
}

[[nodiscard]] bool ShapeTelemetryJsonEnabled() noexcept { return EnvTruthy("ASTRALDB_SHAPE_TELEMETRY_JSON"); }

ShapeRouteDecision ClassifyQueryShape(BytecodeInterpreter &Vm, const Bytecode &Code) noexcept {
	ShapeRouteDecision Out;
	Out.OptConfidence = ConfidenceScorer{}.ScoreBytecode(Code);
	QueryShapeComposition Comp;
	if(InferShapeCompositionFromBytecode(Code, Comp) && Comp.Root != UINT16_MAX) {
		Out.Composition = Comp;
		Out.HasComposition = Comp.Count > 0;
		Out.Shape = Comp.Nodes[Comp.Root].Payload;
		Out.Shape.Kind = BulkQueryKindFromCompositionRoot(Comp);
		Out.Fingerprint = QueryShapeFingerprint128FromComposition(Comp);
	} else if(InferBulkQueryShapeFromBytecode(Code, Out.Shape)) {
		Out.Fingerprint = QueryShapeFingerprint128FromBytecode(Code, Out.Shape);
	} else {
		Out.RouteTier = ShapeRouteTier::FullVm;
		return Out;
	}
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	const bool ReadOnly = AstralDB::BytecodeIsReadOnlyQuery(Code);
	MetadataEligibilityTier Eligibility = MetadataEligibilityTier::Blocked;
	if(Db) {
		const std::string RawKey = !Out.Shape.FactTable.empty() ? Out.Shape.FactTable : Out.Shape.Table;
		const std::string TableKey = ResolveBulkQuerySourceTable(Code, RawKey);
		Db->WithExclusiveBytecodeLock([&]() {
			const auto It = Db->Tables_.find(TableKey);
			if(It != Db->Tables_.end())
				Eligibility = ClassifyMetadataEligibility(It->second.Columnar, ReadOnly);
		});
	}
	Out.EligibilityTier = Eligibility;
	if(ReadOnly && MetadataTierAllowsFastPath(Eligibility)) {
		Out.RouteTier = ShapeRouteTier::MetadataO1;
		return Out;
	}
	if(HasFusedDominantOpcodes(Code) || Out.OptConfidence >= Confidence::High)
		Out.RouteTier = ShapeRouteTier::FusedDominant;
	else if(Out.OptConfidence >= Confidence::Medium)
		Out.RouteTier = ShapeRouteTier::JitAccelerated;
	else
		Out.RouteTier = ShapeRouteTier::FullVm;
	return Out;
}

bool TryExecuteShapeRoute(BytecodeInterpreter &Vm, const Bytecode &Code, const ShapeRouteDecision &Decision) noexcept {
	SetShapeReadOnlyQueryContext(AstralDB::BytecodeIsReadOnlyQuery(Code));
	switch(Decision.RouteTier) {
	case ShapeRouteTier::MetadataO1:
		return TryExecuteDominantBulkQueryMetadata(Vm, Code);
	case ShapeRouteTier::FusedDominant:
		if(TryExecuteDominantWarehouseMegafusionBytecode(Vm, Code))
			return true;
		if(TryExecuteDominantStarJoinGroupBytecode(Vm, Code))
			return true;
		if(TryExecuteDominantStarJoinSelectBytecode(Vm, Code))
			return true;
		if(TryExecuteDominantStarJoinCubeBytecode(Vm, Code))
			return true;
		if(TryExecuteDominantSemistructuredBytecode(Vm, Code))
			return true;
		if(TryExecuteDominantBulkQueryMetadata(Vm, Code))
			return true;
		return TryExecuteDominantAmbBytecode(Vm, Code);
	default:
		return false;
	}
}

void RecordShapeTelemetry(BytecodeInterpreter &Vm, const Bytecode &Code, const ShapeRouteDecision &Decision,
                          const bool MetadataHit) noexcept {
	Database *Db = Vm.PrimaryDatabase();
	if(Db) {
		const std::string RawKey =
		    !Decision.Shape.FactTable.empty() ? Decision.Shape.FactTable : Decision.Shape.Table;
		const std::string TableKey = ResolveBulkQuerySourceTable(Code, RawKey);
		Db->WithExclusiveBytecodeLock([&]() {
			const auto It = Db->Tables_.find(TableKey);
			if(It != Db->Tables_.end())
				RecordObservedQueryShape(It->second.Columnar, Decision.Shape, Decision.Fingerprint);
		});
	}
	if(!ShapeTelemetryEnabled())
		return;
	auto &Stats = Vm.MutableTimeSqlStats().ShapeStats;
	Stats.RouteTier = Decision.RouteTier;
	Stats.EligibilityTier = Decision.EligibilityTier;
	Stats.Fingerprint = Decision.Fingerprint;
	Stats.ShapeKind = Decision.Shape.Kind;
	Stats.MatcherMiss = static_cast<std::uint8_t>(Decision.MatcherMiss);
	Stats.MetadataHit = MetadataHit;
	if(Decision.HasComposition)
		Stats.CompositionNodes = static_cast<std::uint16_t>(Decision.Composition.Count);
	if(Decision.Shape.HasLimit)
		Stats.ObservedLimitK = static_cast<std::uint32_t>(Decision.Shape.Limit);
}

bool TryExecuteReadOnlyViaShapeRouter(BytecodeInterpreter &Vm, const Bytecode &Code) noexcept {
	if(!AstralDB::BytecodeIsReadOnlyQuery(Code))
		return false;
	const ShapeRouteDecision Decision = ClassifyQueryShape(Vm, Code);
	if(Decision.RouteTier == ShapeRouteTier::MetadataO1 || Decision.RouteTier == ShapeRouteTier::FusedDominant) {
		const bool Hit = TryExecuteShapeRoute(Vm, Code, Decision);
		RecordShapeTelemetry(Vm, Code, Decision, Hit);
		return Hit;
	}
	RecordShapeTelemetry(Vm, Code, Decision, false);
	return false;
}

void AppendShapeTelemetryFields(std::ostringstream &O, const BytecodeInterpreter::TimeSqlStats &Stats) noexcept {
	if(!ShapeTelemetryEnabled())
		return;
	const ShapeTelemetry &S = Stats.ShapeStats;
	O << " shape_route=" << ShapeRouteTierName(S.RouteTier);
	O << " shape_eligibility=" << MetadataEligibilityTierName(S.EligibilityTier);
	O << " shape_fp=" << std::hex << S.Fingerprint.Lo << ':' << S.Fingerprint.Hi << std::dec;
	O << " shape_kind=" << BulkQueryKindName(S.ShapeKind);
	if(S.MatcherMiss != 0)
		O << " shape_miss=" << static_cast<unsigned>(S.MatcherMiss);
	if(S.MetadataHit)
		O << " shape_metadata_hit=1";
	if(S.ObservedLimitK > 0)
		O << " shape_limit_k=" << S.ObservedLimitK;
	if(S.CompositionNodes > 0)
		O << " shape_comp_nodes=" << S.CompositionNodes;
	if(ShapeTelemetryJsonEnabled()) {
		O << " shape_telemetry_json={\"route\":\"" << ShapeRouteTierName(S.RouteTier) << "\",\"eligibility\":\""
		  << MetadataEligibilityTierName(S.EligibilityTier) << "\",\"fp_lo\":" << std::hex << S.Fingerprint.Lo
		  << ",\"fp_hi\":" << S.Fingerprint.Hi << std::dec << ",\"kind\":\"" << BulkQueryKindName(S.ShapeKind)
		  << "\",\"metadata_hit\":" << (S.MetadataHit ? "true" : "false");
		if(S.MatcherMiss != 0)
			O << ",\"miss\":" << static_cast<unsigned>(S.MatcherMiss);
		if(S.ObservedLimitK > 0)
			O << ",\"limit_k\":" << S.ObservedLimitK;
		O << "}";
	}
}

} // namespace SQL
} // namespace AstralDB
