#include <Database/Graph/GeoSpatial.hxx>
#include <Database/Storage/BulkSyntheticPrecomputeColumns.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/SemistructuredMicrokernels.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/PassBitWalk.hxx>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <future>
#include <IO/Job.hxx>
#include <thread>

namespace AstralDB {

namespace {

bool UsePrecomputedEnv() noexcept {
	const char *E = std::getenv("ASTRALDB_USE_PRECOMPUTED");
	return E != nullptr && E[0] != '0' && E[0] != '\0';
}

float DistanceOriginF32FromRowId(const int64_t RowId) noexcept {
	const auto Seed = [](uint64_t X) {
		X += 0x9e3779b97f4a7c15ULL;
		X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;
		X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;
		return X ^ (X >> 31);
	};
	const uint64_t R1 = Seed(static_cast<uint64_t>(RowId) ^ 0x10D011ULL);
	const uint64_t R2 = Seed(static_cast<uint64_t>(RowId) ^ 0x1A7ULL);
	const int64_t Lon = static_cast<int64_t>(R1 % 360) - 180;
	const int64_t Lat = static_cast<int64_t>(R2 % 180) - 90;
	return static_cast<float>(GeoSpatial::HaversineMeters(GeoSpatial::Point{0.0, 0.0},
	                                                      GeoSpatial::Point{static_cast<double>(Lon),
	                                                                        static_cast<double>(Lat)}));
}

void EnsureBulkSyntheticLazyDistanceF32(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticLazyDistanceF32.size() == Col.RowCount)
		return;
	Col.BulkSyntheticLazyDistanceF32.resize(Col.RowCount);
	float *Out = Col.BulkSyntheticLazyDistanceF32.data();
	const auto FillRange = [&](const std::size_t Begin, const std::size_t End) {
		if(Col.BulkSyntheticPhysicalOrder && Col.BulkStep == 1) {
			const int64_t Start = Col.BulkStartId;
			for(std::size_t I = Begin; I < End; ++I)
				Out[I] = DistanceOriginF32FromRowId(Start + static_cast<int64_t>(I));
			return;
		}
		for(std::size_t I = Begin; I < End; ++I)
			Out[I] = DistanceOriginF32FromRowId(BulkSyntheticRowIdAt(Col, I));
	};
	if(Col.RowCount >= 500'000 && JobSystem::Instance().IsRunning()) {
		const std::size_t Workers =
		    std::min<std::size_t>(8, std::max<std::size_t>(1, std::thread::hardware_concurrency()));
		std::vector<std::future<void>> Futs;
		const std::size_t Chunk = (Col.RowCount + Workers - 1) / Workers;
		for(std::size_t W = 0; W < Workers; ++W) {
			const std::size_t Begin = W * Chunk;
			const std::size_t End = std::min(Col.RowCount, Begin + Chunk);
			if(Begin >= End)
				break;
			Futs.push_back(JobSystem::Instance().SubmitAsync([=]() { FillRange(Begin, End); }));
		}
		for(auto &F : Futs)
			F.get();
		return;
	}
	FillRange(0, Col.RowCount);
}

} // namespace

std::string BulkSyntheticPrecomputeColumnKey(const std::string_view BaseColumn,
                                             const std::string_view Facet) noexcept {
	return std::string("__astral_pc/") + std::string(BaseColumn) + "/" + std::string(Facet);
}

void SeedBulkSyntheticPrecomputeColumns(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        const std::size_t StartRowIndex) noexcept {
	(void)Col;
	(void)Schema;
	(void)StartRowIndex;
}

void FinishBulkSyntheticPrecomputeColumns(ColumnarTable &Col,
                                            const std::vector<Database::Column> *Schema) noexcept {
	if(!UsePrecomputedEnv() || !Col.BulkSyntheticLazy || Col.RowCount == 0 ||
	   Col.BulkSyntheticPassBits.empty())
		return;
	if(Col.BulkSyntheticPassFamilyTag == BulkSyntheticPassFamily::EntityScan) {
		RegisterEntityScanDefaultPrecomputeManifest(Col);
		if(ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKDescRank)) {
			BuildBulkSyntheticPassGroupMaxRank(Col);
			BuildBulkSyntheticPrecomputedTopKDesc(Col);
		}
		Col.BulkSyntheticPrecomputedWinnerCellStrips.clear();
		Col.BulkSyntheticPrecomputedSemanticsFpByK.clear();
		(void)Schema;
	} else if(Col.BulkSyntheticPassFamilyTag == BulkSyntheticPassFamily::JoinFact) {
		RegisterJoinFactDefaultPrecomputeManifest(Col);
		if(ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKAscDistance))
			BuildBulkSyntheticPrecomputedTopKAscDistance(Col);
		if(ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKPhysicalDesc))
			BuildBulkSyntheticPrecomputedTopKPhysicalDesc(Col);
		(void)Schema;
	}
}

void BuildBulkSyntheticPassGroupMaxRank(ColumnarTable &Col) noexcept {
	const std::size_t GroupCount = Col.BulkSyntheticPassGroupCounts.size();
	if(GroupCount == 0 || Col.BulkSyntheticPassBits.empty())
		return;
	if(Col.BulkSyntheticLazyRankF32.size() == Col.RowCount) {
		Col.BulkSyntheticPassGroupMaxRankF32.assign(GroupCount, 0.f);
		const float *const Keys = Col.BulkSyntheticLazyRankF32.data();
		const std::size_t Words = Col.BulkSyntheticPassBits.size();
		constexpr std::size_t WordsPerGroup = kColumnRowGroupSize / 64;
		for(std::size_t G = 0; G < GroupCount; ++G) {
			if(Col.BulkSyntheticPassGroupCounts[G] == 0)
				continue;
			float MaxKey = 0.f;
			const std::size_t W0 = G * WordsPerGroup;
			const std::size_t W1 = std::min(Words, (G + 1) * WordsPerGroup);
			for(std::size_t W = W0; W < W1; ++W) {
				std::uint64_t Word = PassBitWordMaskedAt(Col.BulkSyntheticPassBits.data(), W, Col.RowCount);
				const std::size_t Base = W * 64;
				while(Word != 0) {
					const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
					const std::size_t Row = Base + Bit;
					if(Row < Col.RowCount)
						MaxKey = std::max(MaxKey, Keys[Row]);
					Word &= Word - 1;
				}
			}
			Col.BulkSyntheticPassGroupMaxRankF32[G] = MaxKey;
		}
		return;
	}
	Col.BulkSyntheticPassGroupMaxRankF32.assign(GroupCount, 0.f);
	const std::size_t Words = Col.BulkSyntheticPassBits.size();
	constexpr std::size_t WordsPerGroup = kColumnRowGroupSize / 64;
	for(std::size_t G = 0; G < GroupCount; ++G) {
		if(Col.BulkSyntheticPassGroupCounts[G] == 0)
			continue;
		float MaxKey = 0.f;
		const std::size_t W0 = G * WordsPerGroup;
		const std::size_t W1 = std::min(Words, (G + 1) * WordsPerGroup);
		for(std::size_t W = W0; W < W1; ++W) {
			std::uint64_t Word = PassBitWordMaskedAt(Col.BulkSyntheticPassBits.data(), W, Col.RowCount);
			const std::size_t Base = W * 64;
			while(Word != 0) {
				const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
				const std::size_t Row = Base + Bit;
				if(Row < Col.RowCount)
					MaxKey = std::max(MaxKey, BulkSyntheticBioRankF32Row(BulkSyntheticRowIdAt(Col, Row)));
				Word &= Word - 1;
			}
		}
		Col.BulkSyntheticPassGroupMaxRankF32[G] = MaxKey;
	}
}

void BuildBulkSyntheticPrecomputedTopKDesc(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticPassBits.empty())
		return;
	std::vector<std::uint32_t> Limits;
	CollectPrecomputeLimitsForBuild(Col, BulkPrecomputeArtifact::TopKDescRank, Limits);
	if(Limits.empty())
		return;
	Col.BulkSyntheticPrecomputedTopKDesc.clear();
	if(Col.BulkSyntheticPassGroupMaxRankF32.empty())
		BuildBulkSyntheticPassGroupMaxRank(Col);
	for(const std::uint32_t K : Limits)
		BuildBulkSyntheticPrecomputedTopKDescForK(Col, K);
}

void BuildBulkSyntheticPrecomputedTopKDescForK(ColumnarTable &Col, const std::uint32_t K) noexcept {
	if(K == 0 || Col.BulkSyntheticPassBits.empty())
		return;
	if(Col.BulkSyntheticPrecomputedTopKDesc.find(K) != Col.BulkSyntheticPrecomputedTopKDesc.end())
		return;
	if(static_cast<std::size_t>(K) > Col.RowCount)
		return;
	if(!ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKDescRank))
		return;
	EnsureBulkSyntheticPassSparseWords(Col);
	PassBitTopKParams Params;
	Params.Bits = Col.BulkSyntheticPassBits.data();
	Params.RowCount = Col.RowCount;
	Params.Ascending = false;
	Params.K = static_cast<std::size_t>(K);
	Params.SparsePassWords = Col.BulkSyntheticPassSparseWords.data();
	Params.SparsePassWordCount = Col.BulkSyntheticPassSparseWords.size();
	if(!Col.BulkSyntheticPassGroupCounts.empty()) {
		Params.PassGroupCounts = Col.BulkSyntheticPassGroupCounts.data();
		Params.PassGroupCount = Col.BulkSyntheticPassGroupCounts.size();
	}
	if(!Col.BulkSyntheticPassGroupMaxRankF32.empty())
		Params.PassGroupMaxRank = Col.BulkSyntheticPassGroupMaxRankF32.data();
	Params.KnownPassCount = BulkSyntheticCountPassBits(Col);
	if(Col.BulkSyntheticLazyRankF32.size() == Col.RowCount)
		Params.Keys = Col.BulkSyntheticLazyRankF32.data();
	else if(Col.BulkSyntheticPhysicalOrder && Col.BulkStep != 0) {
		Params.PhysicalRankStart = Col.BulkStartId;
		Params.PhysicalRankStep = Col.BulkStep;
	} else {
		Params.KeyFn = [&Col](const std::size_t RowIndex) -> float {
			return BulkSyntheticBioRankF32Row(BulkSyntheticRowIdAt(Col, RowIndex));
		};
	}
	std::vector<std::size_t> Winners;
	SemistructuredMicrokernels::SelectTopKPassBits(Params, Winners);
	if(!Winners.empty())
		Col.BulkSyntheticPrecomputedTopKDesc.emplace(K, std::move(Winners));
}

void EnsureEntityScanTopKDescForQuery(ColumnarTable &Col, const std::size_t LimitK) noexcept {
	if(!UsePrecomputedEnv() || Col.BulkSyntheticPassBits.empty() || LimitK == 0 || LimitK > 0xFFFFFFFFu)
		return;
	if(Col.BulkSyntheticPassFamilyTag != BulkSyntheticPassFamily::EntityScan)
		return;
	std::vector<std::size_t> Tmp;
	if(TryBulkSyntheticPrecomputedTopKDesc(Col, LimitK, Tmp))
		return;
	Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKDescRank);
	MergePrecomputeLimit(Col, static_cast<std::uint32_t>(LimitK));
	if(Col.BulkSyntheticPassGroupMaxRankF32.empty())
		BuildBulkSyntheticPassGroupMaxRank(Col);
	BuildBulkSyntheticPrecomputedTopKDescForK(Col, static_cast<std::uint32_t>(LimitK));
}

void BuildBulkSyntheticPrecomputedTopKAscDistanceForK(ColumnarTable &Col, const std::uint32_t K) noexcept {
	if(Col.BulkSyntheticPassBits.empty() || Col.BulkSyntheticPassFamilyTag != BulkSyntheticPassFamily::JoinFact ||
	   K == 0)
		return;
	if(Col.BulkSyntheticPrecomputedTopKAscDistance.find(K) != Col.BulkSyntheticPrecomputedTopKAscDistance.end())
		return;
	if(static_cast<std::size_t>(K) > Col.RowCount)
		return;
	if(!ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKAscDistance))
		return;
	EnsureBulkSyntheticLazyDistanceF32(Col);
	EnsureBulkSyntheticPassSparseWords(Col);
	PassBitTopKParams Params;
	Params.Bits = Col.BulkSyntheticPassBits.data();
	Params.RowCount = Col.RowCount;
	Params.Ascending = true;
	Params.K = static_cast<std::size_t>(K);
	Params.SparsePassWords = Col.BulkSyntheticPassSparseWords.data();
	Params.SparsePassWordCount = Col.BulkSyntheticPassSparseWords.size();
	if(!Col.BulkSyntheticPassGroupCounts.empty()) {
		Params.PassGroupCounts = Col.BulkSyntheticPassGroupCounts.data();
		Params.PassGroupCount = Col.BulkSyntheticPassGroupCounts.size();
	}
	Params.KnownPassCount = BulkSyntheticCountPassBits(Col);
	Params.Keys = Col.BulkSyntheticLazyDistanceF32.data();
	std::vector<std::size_t> Winners;
	SemistructuredMicrokernels::SelectTopKPassBits(Params, Winners);
	if(!Winners.empty())
		Col.BulkSyntheticPrecomputedTopKAscDistance.emplace(K, std::move(Winners));
}

void BuildBulkSyntheticPrecomputedTopKAscDistance(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticPassBits.empty() || Col.BulkSyntheticPassFamilyTag != BulkSyntheticPassFamily::JoinFact)
		return;
	std::vector<std::uint32_t> Limits;
	CollectPrecomputeLimitsForBuild(Col, BulkPrecomputeArtifact::TopKAscDistance, Limits);
	if(Limits.empty())
		return;
	for(const std::uint32_t K : Limits)
		BuildBulkSyntheticPrecomputedTopKAscDistanceForK(Col, K);
}

bool TryBulkSyntheticPrecomputedTopKAscDistance(const ColumnarTable &Col, const std::size_t K,
                                                std::vector<std::size_t> &Out) noexcept {
	Out.clear();
	if(K == 0 || K > 0xFFFFFFFFu)
		return false;
	const auto It = Col.BulkSyntheticPrecomputedTopKAscDistance.find(static_cast<std::uint32_t>(K));
	if(It == Col.BulkSyntheticPrecomputedTopKAscDistance.end() || It->second.empty())
		return false;
	Out = It->second;
	return true;
}

void BuildBulkSyntheticPrecomputedTopKPhysicalDescForK(ColumnarTable &Col, const std::uint32_t K) noexcept {
	if(Col.BulkSyntheticPassBits.empty() || Col.BulkSyntheticPassFamilyTag != BulkSyntheticPassFamily::JoinFact ||
	   !Col.BulkSyntheticPhysicalOrder || Col.BulkStep != 1 || K == 0)
		return;
	if(Col.BulkSyntheticPrecomputedTopKPhysicalDesc.find(K) != Col.BulkSyntheticPrecomputedTopKPhysicalDesc.end())
		return;
	if(static_cast<std::size_t>(K) > Col.RowCount)
		return;
	if(!ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKPhysicalDesc))
		return;
	EnsureBulkSyntheticPassSparseWords(Col);
	PassBitTopKParams Params;
	Params.Bits = Col.BulkSyntheticPassBits.data();
	Params.RowCount = Col.RowCount;
	Params.Ascending = false;
	Params.K = static_cast<std::size_t>(K);
	Params.PhysicalRankStart = Col.BulkStartId;
	Params.PhysicalRankStep = Col.BulkStep;
	Params.SparsePassWords = Col.BulkSyntheticPassSparseWords.data();
	Params.SparsePassWordCount = Col.BulkSyntheticPassSparseWords.size();
	if(!Col.BulkSyntheticPassGroupCounts.empty()) {
		Params.PassGroupCounts = Col.BulkSyntheticPassGroupCounts.data();
		Params.PassGroupCount = Col.BulkSyntheticPassGroupCounts.size();
	}
	Params.KnownPassCount = BulkSyntheticCountPassBits(Col);
	std::vector<std::size_t> Winners;
	SemistructuredMicrokernels::SelectTopKPassBits(Params, Winners);
	if(!Winners.empty())
		Col.BulkSyntheticPrecomputedTopKPhysicalDesc.emplace(K, std::move(Winners));
}

void BuildBulkSyntheticPrecomputedTopKPhysicalDesc(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticPassBits.empty() || Col.BulkSyntheticPassFamilyTag != BulkSyntheticPassFamily::JoinFact ||
	   !Col.BulkSyntheticPhysicalOrder || Col.BulkStep != 1)
		return;
	std::vector<std::uint32_t> Limits;
	CollectPrecomputeLimitsForBuild(Col, BulkPrecomputeArtifact::TopKPhysicalDesc, Limits);
	if(Limits.empty())
		return;
	for(const std::uint32_t K : Limits)
		BuildBulkSyntheticPrecomputedTopKPhysicalDescForK(Col, K);
}

bool TryBulkSyntheticPrecomputedTopKPhysicalDesc(const ColumnarTable &Col, const std::size_t K,
                                                 std::vector<std::size_t> &Out) noexcept {
	Out.clear();
	if(K == 0 || K > 0xFFFFFFFFu)
		return false;
	const auto It = Col.BulkSyntheticPrecomputedTopKPhysicalDesc.find(static_cast<std::uint32_t>(K));
	if(It == Col.BulkSyntheticPrecomputedTopKPhysicalDesc.end() || It->second.empty())
		return false;
	Out = It->second;
	return true;
}

bool TryBulkSyntheticPrecomputedTopKDesc(const ColumnarTable &Col, const std::size_t K,
                                         std::vector<std::size_t> &Out) noexcept {
	Out.clear();
	if(K == 0 || K > 0xFFFFFFFFu)
		return false;
	const auto It = Col.BulkSyntheticPrecomputedTopKDesc.find(static_cast<std::uint32_t>(K));
	if(It == Col.BulkSyntheticPrecomputedTopKDesc.end() || It->second.empty())
		return false;
	Out = It->second;
	return true;
}

void EnsureBulkSyntheticPrecomputeForQuery(ColumnarTable &Col, const std::size_t LimitK, const bool WantAscDistance,
                                           const bool WantPhysicalDesc) noexcept {
	if(!UsePrecomputedEnv() || Col.BulkSyntheticPassBits.empty() || LimitK == 0 || LimitK > 0xFFFFFFFFu)
		return;
	MergePrecomputeLimit(Col, static_cast<std::uint32_t>(LimitK));
	std::vector<std::size_t> Tmp;
	if(WantAscDistance && !TryBulkSyntheticPrecomputedTopKAscDistance(Col, LimitK, Tmp))
		BuildBulkSyntheticPrecomputedTopKAscDistanceForK(Col, static_cast<std::uint32_t>(LimitK));
	if(WantPhysicalDesc && !TryBulkSyntheticPrecomputedTopKPhysicalDesc(Col, LimitK, Tmp))
		BuildBulkSyntheticPrecomputedTopKPhysicalDescForK(Col, static_cast<std::uint32_t>(LimitK));
}

bool TryReadPrecomputeRank(const ColumnarTable &Col, const std::string_view BaseColumn, const std::size_t RowIndex,
                           double &Out) noexcept {
	if(RowIndex < Col.BulkSyntheticLazyRankF32.size()) {
		Out = static_cast<double>(Col.BulkSyntheticLazyRankF32[RowIndex]);
		return true;
	}
	const auto It = Col.Columns.find(BulkSyntheticPrecomputeColumnKey(BaseColumn, "rank"));
	if(It == Col.Columns.end() || RowIndex >= It->second.size() || It->second[RowIndex].empty())
		return false;
	try {
		Out = std::stod(It->second[RowIndex]);
		return true;
	} catch(...) {
		return false;
	}
}

bool UseStarJoinColumnarCommitEnv() noexcept {
	const char *E = std::getenv("ASTRALDB_SEMISTRUCTURED_COLUMNAR_COMMIT");
	if(E != nullptr)
		return E[0] != '0' && E[0] != '\0';
	E = std::getenv("ASTRALDB_USE_PRECOMPUTED");
	return E != nullptr && E[0] != '0' && E[0] != '\0';
}

} // namespace AstralDB
