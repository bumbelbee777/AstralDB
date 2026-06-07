#include <Database/Storage/BulkSyntheticDerive.hxx>

#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/StarJoinCubeBulk.hxx>

#include <cstdlib>

namespace AstralDB {

namespace {

thread_local bool g_ShapeReadOnlyQueryContext = false;

[[nodiscard]] bool EnvTruthy(const char *Name) noexcept {
	const char *E = std::getenv(Name);
	return E != nullptr && E[0] != '0' && E[0] != '\0';
}

[[nodiscard]] std::uint32_t ClosedFormPassRatePermille(const std::uint64_t KindMask) noexcept {
	if(KindMask == 0)
		return 1000;
	std::uint32_t Num = 1000;
	std::uint32_t Den = 1000;
	if((KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractEq)) != 0) {
		Num *= 670;
		Den *= 1000;
	}
	if((KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractIn)) != 0) {
		Num *= 400;
		Den *= 1000;
	}
	if((KindMask & PredicateKindBit(PredicateKindFlag::RegexMatch)) != 0) {
		Num *= 250;
		Den *= 1000;
	}
	return static_cast<std::uint32_t>((static_cast<std::uint64_t>(Num) * 1000 + Den / 2) / Den);
}

[[nodiscard]] bool KindMaskFullyPassing(const std::uint64_t KindMask) noexcept {
	const std::uint64_t Restrictive = KindMask & (PredicateKindBit(PredicateKindFlag::JsonExtractEq) |
	                                              PredicateKindBit(PredicateKindFlag::JsonExtractIn) |
	                                              PredicateKindBit(PredicateKindFlag::RegexMatch));
	return KindMask != 0 && Restrictive == 0;
}

} // namespace

bool LegacyPassBitsEnabled() noexcept {
	return EnvTruthy("ASTRALDB_LEGACY_PASS_BITS");
}

bool MetadataOnlyInsertEnabled() noexcept {
	if(LegacyPassBitsEnabled())
		return false;
	return !EnvTruthy("ASTRALDB_DISABLE_METADATA_INSERT");
}

bool MetadataFastPathDemoEnabled() noexcept {
	return EnvTruthy("ASTRALDB_METADATA_FASTPATH_DEMO");
}

bool MetadataStatInjectionEligible(const ColumnarTable &Col) noexcept {
	return MetadataTierAllowsFastPath(ClassifyMetadataEligibility(Col, g_ShapeReadOnlyQueryContext));
}

void SetShapeReadOnlyQueryContext(const bool ReadOnly) noexcept { g_ShapeReadOnlyQueryContext = ReadOnly; }

bool ShapeReadOnlyQueryContext() noexcept { return g_ShapeReadOnlyQueryContext; }

MetadataEligibilityTier ClassifyMetadataEligibility(const ColumnarTable &Col,
                                                    const bool ReadOnlyQuery) noexcept {
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0)
		return MetadataEligibilityTier::Blocked;
	if(!Col.BulkSyntheticPassBits.empty() || !Col.BulkSyntheticPrecomputedQ1TailRows.empty() ||
	   !Col.BulkSyntheticPrecomputedTopKDesc.empty())
		return MetadataEligibilityTier::DensePrecompute;
	if(Col.BulkSyntheticJoinFactPrecomputeJob != nullptr)
		return MetadataEligibilityTier::PrecomputePending;
	if(!Col.BulkSyntheticMetadataOnly)
		return MetadataEligibilityTier::DensePrecompute;
	if(ReadOnlyQuery)
		return MetadataEligibilityTier::ReadOnlyStatInject;
	if(MetadataFastPathDemoEnabled())
		return MetadataEligibilityTier::DemoStatInject;
	return MetadataEligibilityTier::Blocked;
}

bool MetadataTierAllowsFastPath(const MetadataEligibilityTier Tier) noexcept {
	switch(Tier) {
	case MetadataEligibilityTier::DensePrecompute:
	case MetadataEligibilityTier::ReadOnlyStatInject:
	case MetadataEligibilityTier::DemoStatInject:
		return true;
	default:
		return false;
	}
}

void RegisterSyntheticMetadata(ColumnarTable &Col, const BulkSyntheticPassFamily Family, const std::uint64_t KindMask,
                               const int64_t FkModA, const int64_t FkModB) noexcept {
	Col.BulkSyntheticMetadataOnly = true;
	Col.BulkSyntheticPassFamilyTag = Family;
	Col.BulkSyntheticPassKindMask = KindMask;
	if(Family == BulkSyntheticPassFamily::JoinFact) {
		if(Col.BulkSyntheticStarCubeHavingMin <= 0)
			Col.BulkSyntheticStarCubeHavingMin = 101;
		Col.BulkSyntheticFkCustMod = 997;
		Col.BulkSyntheticFkProdMod = 1000;
		Col.BulkSyntheticCachedFkModA = 997;
		Col.BulkSyntheticCachedFkModB = 1000;
	} else {
		if(FkModA > 0)
			Col.BulkSyntheticFkCustMod = FkModA;
		if(FkModB > 0)
			Col.BulkSyntheticFkProdMod = FkModB;
		if(FkModA > 0)
			Col.BulkSyntheticCachedFkModA = FkModA;
		if(FkModB > 0)
			Col.BulkSyntheticCachedFkModB = FkModB;
	}
	Col.BulkSyntheticPassRatePermille = ClosedFormPassRatePermille(KindMask);
	Col.BulkSyntheticCachedPassKindMask = KindMask;
	EnsureMetadataPassCoverage(Col);
}

void EnsureMetadataPassCoverage(ColumnarTable &Col) noexcept {
	if(Col.RowCount == 0)
		return;
	if(Col.BulkSyntheticPassFamilyTag == BulkSyntheticPassFamily::JoinFact &&
	   KindMaskFullyPassing(Col.BulkSyntheticPassKindMask)) {
		Col.BulkSyntheticPassAllRows = true;
		Col.BulkSyntheticPassRatePermille = 1000;
		return;
	}
	if(Col.BulkSyntheticPassRatePermille >= 1000)
		Col.BulkSyntheticPassAllRows = true;
}

std::uint32_t DerivePassRatePermille(const ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticPassAllRows)
		return 1000;
	return Col.BulkSyntheticPassRatePermille > 0 ? Col.BulkSyntheticPassRatePermille : 1000;
}

std::uint64_t BulkSyntheticLiveRowCount(const ColumnarTable &Col) noexcept {
	if(Col.RowCount == 0)
		return 0;
	if(Col.BulkSyntheticDeleteBits.empty())
		return static_cast<std::uint64_t>(Col.RowCount);
	if(Col.BulkSyntheticDeletedCount > 0)
		return static_cast<std::uint64_t>(Col.RowCount) - Col.BulkSyntheticDeletedCount;
	std::uint64_t Deleted = 0;
	for(std::size_t I = 0; I < Col.RowCount; ++I) {
		if(BulkSyntheticRowIsDeleted(Col, I))
			++Deleted;
	}
	return static_cast<std::uint64_t>(Col.RowCount) - Deleted;
}

std::optional<std::uint64_t> DeriveLazyBulkIdRangeMatchCount(
    const ColumnarTable &Col, const std::vector<std::vector<FilterPredicateTriple>> &Filters) noexcept {
	if(!Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder || Col.BulkStep != 1 || Filters.size() != 1)
		return std::nullopt;
	int64_t Lo = 0;
	int64_t Hi = 0;
	bool HasLo = false;
	bool HasHi = false;
	for(const auto &[ColName, Op, Val] : Filters[0]) {
		if(ColName != "id")
			return std::nullopt;
		try {
			if(Op == ">=" || Op == ">") {
				Lo = std::stoll(Val);
				HasLo = true;
			} else if(Op == "<=" || Op == "<") {
				Hi = Op == "<" ? std::stoll(Val) - 1 : std::stoll(Val);
				HasHi = true;
			} else {
				return std::nullopt;
			}
		} catch(...) {
			return std::nullopt;
		}
	}
	if(!HasLo || !HasHi || Hi < Lo)
		return std::nullopt;
	const int64_t Start = Col.BulkStartId;
	const std::size_t I0 = Lo <= Start ? 0 : static_cast<std::size_t>(Lo - Start);
	const std::size_t I1 = static_cast<std::size_t>(Hi - Start + 1);
	if(I0 >= Col.RowCount)
		return 0;
	const std::size_t End = std::min(Col.RowCount, I1);
	if(I0 >= End)
		return 0;
	std::uint64_t Live = static_cast<std::uint64_t>(End - I0);
	if(!Col.BulkSyntheticDeleteBits.empty()) {
		for(std::size_t I = I0; I < End; ++I) {
			if(BulkSyntheticRowIsDeleted(Col, I))
				--Live;
		}
	}
	return Live;
}

std::uint64_t DerivePassCount(const ColumnarTable &Col) noexcept {
	const std::uint64_t Live = BulkSyntheticLiveRowCount(Col);
	if(Live == 0)
		return 0;
	if(!Col.BulkSyntheticPassBits.empty())
		return BulkSyntheticCountPassBits(Col);
	if(Col.BulkSyntheticPassAllRows)
		return Live;
	const std::uint64_t Rate = DerivePassRatePermille(Col);
	return (Live * Rate + 999) / 1000;
}

void DeriveResidueClassCubeSurvivors(ColumnarTable &Col) noexcept {
	BuildStarJoinCubeResidueClassSurvivors(Col);
}

} // namespace AstralDB
