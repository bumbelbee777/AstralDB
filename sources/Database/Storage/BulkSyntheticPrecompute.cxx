#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticDerive.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/StarJoinCubeBulk.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>

#include <Database/Storage/PassBitWalk.hxx>

#include <Database/Storage/PredicateKind.hxx>

#include <IO/Job.hxx>

#include <bit>
#include <future>
#include <thread>



namespace AstralDB {



std::uint64_t BulkSyntheticPredKindBit(const BulkSyntheticPredKind Kind) noexcept {

	return static_cast<std::uint64_t>(Kind);

}



std::uint64_t BulkSyntheticPassKindsEvaluated(const BulkSyntheticPassFamily Family) noexcept {

	return PassKindsEvaluatedForFamily(Family);

}



std::uint64_t BulkSyntheticDetectPassKindMaskFromSchema(const std::vector<Database::Column> &Schema) noexcept {

	return DetectPredicateKindsFromSchema(Schema);

}



std::uint64_t BulkSyntheticDetectPassKindMask(const BulkSyntheticPassFamily Family,

                                               const std::vector<Database::Column> &PrimarySchema,

                                               const std::vector<Database::Column> *LinkedSchema) noexcept {

	if(Family == BulkSyntheticPassFamily::None)

		return 0;

	std::uint64_t Mask = BulkSyntheticDetectPassKindMaskFromSchema(PrimarySchema);

	if(Family == BulkSyntheticPassFamily::JoinFact && LinkedSchema != nullptr)

		Mask |= BulkSyntheticDetectPassKindMaskFromSchema(*LinkedSchema);

	if(Family == BulkSyntheticPassFamily::JoinFact) {
		for(const Database::Column &Co : PrimarySchema) {
			if(ClassifySqlStorage(Co) == SqlStorageKind::Json)
				Mask |= PredicateKindBit(PredicateKindFlag::JsonExtractIn);
		}
	}

	return Mask;

}



std::uint64_t BulkSyntheticClassifyPredicateKind(const std::string &Col, const std::string &Op, const std::string &Rhs,

                                                 const Database::Column *ColDef) {

	return DetectPredicateKindForTriple(Col, Op, Rhs, ColDef, false);

}



std::uint64_t BulkSyntheticDnfRequiredKindMask(const BulkWhereDnf &Dnf,

                                               const std::vector<Database::Column> &PrimarySchema,

                                               const std::vector<Database::Column> *LinkedSchema) {

	return BuildQueryMaskFromDnf(Dnf, PrimarySchema, LinkedSchema, nullptr, {});

}



bool BulkSyntheticRowPassesKindMask(const BulkSyntheticPassFamily Family, const int64_t PrimaryRowId,

                                    const int64_t LinkedRowId, const std::uint64_t KindMask) noexcept {

	const bool PrimaryJson = (KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractEq)) != 0 ||
	                         (KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractIn)) != 0;

	const bool ReviewText = (KindMask & PredicateKindBit(PredicateKindFlag::RegexMatch)) != 0;

	return SyntheticRowPassesPredicateMask(Family, PrimaryRowId, LinkedRowId, KindMask, PrimaryJson, true, ReviewText);

}

void EnsureJoinFactFkLuts(ColumnarTable &Col, const int64_t FkModA, const int64_t FkModB) noexcept {
	if(FkModA <= 0 || FkModB <= 0)
		return;
	if(Col.BulkSyntheticCountryLut.size() != static_cast<std::size_t>(FkModA)) {
		Col.BulkSyntheticCountryLut.resize(static_cast<std::size_t>(FkModA));
		for(int64_t Rid = 1; Rid <= FkModA; ++Rid)
			Col.BulkSyntheticCountryLut[static_cast<std::size_t>(Rid - 1)] = BulkSyntheticCountryIndex(Rid);
	}
	if(Col.BulkSyntheticCategoryLut.size() != static_cast<std::size_t>(FkModB)) {
		Col.BulkSyntheticCategoryLut.resize(static_cast<std::size_t>(FkModB));
		for(int64_t Rid = 1; Rid <= FkModB; ++Rid)
			Col.BulkSyntheticCategoryLut[static_cast<std::size_t>(Rid - 1)] = BulkSyntheticCategoryIndex(Rid);
	}
}

namespace {

void FillPassBitsAllRows(ColumnarTable &Col) {
	const std::size_t RowCount = Col.RowCount;
	const std::size_t Words = Col.BulkSyntheticPassBits.size();
	for(std::size_t W = 0; W + 1 < Words; ++W)
		Col.BulkSyntheticPassBits[W] = ~0ULL;
	if(Words > 0)
		Col.BulkSyntheticPassBits[Words - 1] = PassBitWordMaskedAt(Col.BulkSyntheticPassBits.data(), Words - 1, RowCount);
	const std::size_t GroupCount = Col.BulkSyntheticPassGroupCounts.size();
	for(std::size_t G = 0; G < GroupCount; ++G) {
		const std::size_t GStart = G * kColumnRowGroupSize;
		const std::size_t GEnd = std::min(RowCount, GStart + kColumnRowGroupSize);
		Col.BulkSyntheticPassGroupCounts[G] = static_cast<std::uint32_t>(GEnd - GStart);
	}
	Col.BulkSyntheticPassAllRows = true;
}

[[nodiscard]] bool JoinFactKindMaskFullyPassing(const std::uint64_t KindMask) noexcept {
	const std::uint64_t Restrictive = KindMask & (PredicateKindBit(PredicateKindFlag::JsonExtractEq) |
	                                              PredicateKindBit(PredicateKindFlag::JsonExtractIn) |
	                                              PredicateKindBit(PredicateKindFlag::RegexMatch));
	return KindMask != 0 && Restrictive == 0;
}

[[nodiscard]] std::size_t PassBitWorkerCount(const std::size_t RowCount) noexcept {
	if(RowCount < 64'000)
		return 1;
	return std::min<std::size_t>(32, std::max<std::size_t>(4, std::thread::hardware_concurrency()));
}

void FinalizeJoinFactRowLuts(ColumnarTable &Col, const int64_t FkModA, const int64_t FkModB) noexcept {
	if(Col.RowCount == 0 || FkModA <= 0 || FkModB <= 0)
		return;
	EnsureJoinFactFkLuts(Col, FkModA, FkModB);
	const std::size_t RowCount = Col.RowCount;
	Col.BulkSyntheticJoinGroupSlotByRow.assign(RowCount, 0);
	Col.BulkSyntheticAmountByRow.assign(RowCount, 0.0);
	for(std::size_t Oi = 0; Oi < RowCount; ++Oi) {
		if(!Col.BulkSyntheticPassAllRows && !BulkSyntheticPassBitAt(Col, Oi))
			continue;
		const int64_t FactRowId = BulkSyntheticRowIdAt(Col, Oi);
		const int64_t LinkedRowId = ((FactRowId - 1) % FkModA) + 1;
		const int64_t SecondDimRowId = ((FactRowId - 1) % FkModB) + 1;
		Col.BulkSyntheticJoinGroupSlotByRow[Oi] =
		    BulkSyntheticWarehouseSlotFromCol(Col, LinkedRowId, SecondDimRowId);
		Col.BulkSyntheticAmountByRow[Oi] = BulkSyntheticDecimalFromRowId(FactRowId);
	}
}

void BuildJoinFactPassBitsParallel(ColumnarTable &Col, const std::uint64_t KindMask, const int64_t FkModA,
                                   const int64_t FkModB, const bool WantCube) {
	const bool PrimaryJson = (KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractEq)) != 0 ||
	                         (KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractIn)) != 0;
	const bool ReviewText = (KindMask & PredicateKindBit(PredicateKindFlag::RegexMatch)) != 0;
	const std::size_t RowCount = Col.RowCount;
	const std::size_t WordCount = Col.BulkSyntheticPassBits.size();
	const std::size_t GroupCount = Col.BulkSyntheticPassGroupCounts.size();
	const std::size_t Workers = PassBitWorkerCount(RowCount);
	struct TilePartial {
		std::vector<std::uint64_t> Words;
		std::vector<std::uint32_t> GroupCounts;
		std::vector<std::uint8_t> PassSlots;
		std::vector<double> PassAmounts;
		std::vector<std::uint32_t> PassRowIndex;
	};
	const auto ProcessTile = [&](const std::size_t RowBegin, const std::size_t RowEnd, TilePartial &Local) {
		Local.Words.assign(WordCount, 0);
		Local.GroupCounts.assign(GroupCount, 0);
		for(std::size_t Oi = RowBegin; Oi < RowEnd; ++Oi) {
			const int64_t FactRowId = BulkSyntheticRowIdAt(Col, Oi);
			const int64_t LinkedRowId = ((FactRowId - 1) % FkModA) + 1;
			const int64_t SecondDimRowId = ((FactRowId - 1) % FkModB) + 1;
			std::uint8_t SlotKey = 0;
			double Amount = 0.0;
			if(WantCube) {
				SlotKey = BulkSyntheticWarehouseSlotFromCol(Col, LinkedRowId, SecondDimRowId);
				Amount = BulkSyntheticDecimalFromRowId(FactRowId);
			}
			if(!SyntheticRowPassesPredicateMask(BulkSyntheticPassFamily::JoinFact, FactRowId, LinkedRowId, KindMask,
			                                  PrimaryJson, true, ReviewText))
				continue;
			const std::size_t Word = Oi >> 6;
			const std::size_t Bit = Oi & 63;
			Local.Words[Word] |= 1ULL << Bit;
			++Local.GroupCounts[Oi / kColumnRowGroupSize];
			if(WantCube) {
				Local.PassSlots.push_back(SlotKey);
				Local.PassAmounts.push_back(Amount);
				Local.PassRowIndex.push_back(static_cast<std::uint32_t>(Oi));
			}
		}
	};
	const auto MergeCubeUpdates = [&](const TilePartial &Local) {
		for(std::size_t I = 0; I < Local.PassRowIndex.size(); ++I) {
			const std::uint32_t Oi = Local.PassRowIndex[I];
			const int64_t FactRowId = BulkSyntheticRowIdAt(Col, Oi);
			UpdateStarJoinCubePrecompute(Col, FactRowId, Local.PassSlots[I], Local.PassAmounts[I]);
		}
	};
	const auto MergeTile = [&](TilePartial &Local) {
		for(std::size_t W = 0; W < WordCount; ++W)
			Col.BulkSyntheticPassBits[W] |= Local.Words[W];
		for(std::size_t G = 0; G < GroupCount; ++G)
			Col.BulkSyntheticPassGroupCounts[G] += Local.GroupCounts[G];
		if(WantCube) {
			MergeCubeUpdates(Local);
			Col.BulkSyntheticPassSlots.insert(Col.BulkSyntheticPassSlots.end(), Local.PassSlots.begin(),
			                                  Local.PassSlots.end());
			Col.BulkSyntheticPassAmounts.insert(Col.BulkSyntheticPassAmounts.end(), Local.PassAmounts.begin(),
			                                      Local.PassAmounts.end());
			Col.BulkSyntheticPassRowIndex.insert(Col.BulkSyntheticPassRowIndex.end(), Local.PassRowIndex.begin(),
			                                     Local.PassRowIndex.end());
		}
	};
	{
		SemistructuredProfileScope Scope(Workers > 1 ? "bulk_pass_bits_parallel" : "bulk_pass_bits_serial");
		(void)Scope;
		if(WantCube) {
			EnsureJoinFactFkLuts(Col, FkModA, FkModB);
			Col.BulkSyntheticPassSlots.clear();
			Col.BulkSyntheticPassAmounts.clear();
			Col.BulkSyntheticPassRowIndex.clear();
			Col.BulkSyntheticJoinGroupSlotByRow.clear();
			Col.BulkSyntheticAmountByRow.clear();
			InitStarJoinCubePrecompute(Col);
		}
		if(Col.BulkSyntheticPassAllRows && WantCube) {
			BuildStarJoinCubePrecomputeParallel(Col);
			Col.BulkSyntheticStarCubeReady = true;
			FinalizeJoinFactRowLuts(Col, FkModA, FkModB);
			BuildBulkSyntheticPassSparseWords(Col);
			return;
		}
		if(Workers <= 1) {
			TilePartial Local;
			ProcessTile(0, RowCount, Local);
			MergeTile(Local);
		} else if(JobSystem::Instance().IsRunning()) {
			std::vector<std::future<TilePartial>> Futs;
			Futs.reserve(Workers);
			const std::size_t Chunk = (RowCount + Workers - 1) / Workers;
			for(std::size_t W = 0; W < Workers; ++W) {
				const std::size_t Begin = W * Chunk;
				const std::size_t End = std::min(RowCount, Begin + Chunk);
				if(Begin >= End)
					break;
				Futs.push_back(JobSystem::Instance().SubmitAsync([&, Begin, End]() {
					TilePartial Local;
					ProcessTile(Begin, End, Local);
					return Local;
				}));
			}
			for(auto &F : Futs) {
				TilePartial Local = F.get();
				MergeTile(Local);
			}
		} else {
			std::vector<std::thread> Pool;
			std::vector<TilePartial> Partials(Workers);
			const std::size_t Chunk = (RowCount + Workers - 1) / Workers;
			Pool.reserve(Workers);
			for(std::size_t W = 0; W < Workers; ++W) {
				const std::size_t Begin = W * Chunk;
				const std::size_t End = std::min(RowCount, Begin + Chunk);
				if(Begin >= End)
					break;
				Pool.emplace_back([&, W, Begin, End]() { ProcessTile(Begin, End, Partials[W]); });
			}
			for(std::thread &T : Pool)
				T.join();
			for(std::size_t W = 0; W < Pool.size(); ++W)
				MergeTile(Partials[W]);
		}
	}
	if(WantCube) {
		Col.BulkSyntheticStarCubeReady = true;
		FinalizeJoinFactRowLuts(Col, FkModA, FkModB);
	}
	BuildBulkSyntheticPassSparseWords(Col);
}

void BuildEntityScanPassBitsParallel(ColumnarTable &Col, const std::uint64_t KindMask) {
	const bool PrimaryJson = (KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractEq)) != 0;
	const std::size_t RowCount = Col.RowCount;
	const std::size_t WordCount = Col.BulkSyntheticPassBits.size();
	const std::size_t GroupCount = Col.BulkSyntheticPassGroupCounts.size();
	const std::size_t Workers = PassBitWorkerCount(RowCount);
	const auto ProcessTile = [&](const std::size_t RowBegin, const std::size_t RowEnd, std::vector<std::uint64_t> &Words,
	                             std::vector<std::uint32_t> &GroupCounts) {
		Words.assign(WordCount, 0);
		GroupCounts.assign(GroupCount, 0);
		for(std::size_t Oi = RowBegin; Oi < RowEnd; ++Oi) {
			const int64_t RowId = BulkSyntheticRowIdAt(Col, Oi);
			if(!SyntheticRowPassesPredicateMask(BulkSyntheticPassFamily::EntityScan, RowId, RowId, KindMask, PrimaryJson,
			                                  false, false))
				continue;
			const std::size_t Word = Oi >> 6;
			const std::size_t Bit = Oi & 63;
			Words[Word] |= 1ULL << Bit;
			++GroupCounts[Oi / kColumnRowGroupSize];
		}
	};
	const auto MergeTile = [&](const std::vector<std::uint64_t> &Words, const std::vector<std::uint32_t> &GroupCounts) {
		for(std::size_t W = 0; W < WordCount; ++W)
			Col.BulkSyntheticPassBits[W] |= Words[W];
		for(std::size_t G = 0; G < GroupCount; ++G)
			Col.BulkSyntheticPassGroupCounts[G] += GroupCounts[G];
	};
	if(Workers <= 1) {
		std::vector<std::uint64_t> Words;
		std::vector<std::uint32_t> GroupCounts;
		ProcessTile(0, RowCount, Words, GroupCounts);
		MergeTile(Words, GroupCounts);
	} else if(JobSystem::Instance().IsRunning()) {
		std::vector<std::future<std::pair<std::vector<std::uint64_t>, std::vector<std::uint32_t>>>> Futs;
		const std::size_t Chunk = (RowCount + Workers - 1) / Workers;
		for(std::size_t W = 0; W < Workers; ++W) {
			const std::size_t Begin = W * Chunk;
			const std::size_t End = std::min(RowCount, Begin + Chunk);
			if(Begin >= End)
				break;
			Futs.push_back(JobSystem::Instance().SubmitAsync([&, Begin, End]() {
				std::vector<std::uint64_t> Words;
				std::vector<std::uint32_t> GroupCounts;
				ProcessTile(Begin, End, Words, GroupCounts);
				return std::make_pair(std::move(Words), std::move(GroupCounts));
			}));
		}
		for(auto &F : Futs) {
			auto [Words, GroupCounts] = F.get();
			MergeTile(Words, GroupCounts);
		}
	} else {
		std::vector<std::thread> Pool;
		std::vector<std::vector<std::uint64_t>> WordParts(Workers);
		std::vector<std::vector<std::uint32_t>> GroupParts(Workers);
		const std::size_t Chunk = (RowCount + Workers - 1) / Workers;
		for(std::size_t W = 0; W < Workers; ++W) {
			const std::size_t Begin = W * Chunk;
			const std::size_t End = std::min(RowCount, Begin + Chunk);
			if(Begin >= End)
				break;
			Pool.emplace_back([&, W, Begin, End]() { ProcessTile(Begin, End, WordParts[W], GroupParts[W]); });
		}
		for(std::thread &T : Pool)
			T.join();
		for(std::size_t W = 0; W < Pool.size(); ++W)
			MergeTile(WordParts[W], GroupParts[W]);
	}
	BuildBulkSyntheticPassSparseWords(Col);
}

} // namespace

void BuildBulkSyntheticPassBits(ColumnarTable &Col, const BulkSyntheticPassFamily Family, std::uint64_t KindMask,

                                const int64_t FkModA, const int64_t FkModB) {

	KindMask &= BulkSyntheticPassKindsEvaluated(Family);

	if(KindMask == 0 || Col.RowCount == 0 || Family == BulkSyntheticPassFamily::None)

		return;

	Col.BulkSyntheticPassFamilyTag = Family;

	Col.BulkSyntheticPassKindMask = KindMask;

	const std::size_t Words = (Col.RowCount + 63) / 64;

	Col.BulkSyntheticPassBits.assign(Words, 0);

	const std::size_t GroupCount = (Col.RowCount + kColumnRowGroupSize - 1) / kColumnRowGroupSize;

	Col.BulkSyntheticPassGroupCounts.assign(GroupCount, 0);



	const bool PrimaryJson = (KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractEq)) != 0 ||
	                         (KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractIn)) != 0;

	const bool ReviewText = (KindMask & PredicateKindBit(PredicateKindFlag::RegexMatch)) != 0;



	if(Family == BulkSyntheticPassFamily::JoinFact && FkModA > 0 && FkModB > 0) {

		Col.BulkSyntheticFkCustMod = FkModA;

		Col.BulkSyntheticFkProdMod = FkModB;

		if(Col.BulkSyntheticStarCubeHavingMin <= 0)
			Col.BulkSyntheticStarCubeHavingMin = 101;
		const bool WantCube = ArtifactMaskRequests(Col, BulkPrecomputeArtifact::StarJoinCube) ||
		                      ArtifactMaskRequests(Col, BulkPrecomputeArtifact::AnalyticCubeSurvivors);
		if(!WantCube)
			Col.BulkSyntheticPrecomputeArtifactMask &=
			    ~(BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::StarJoinCube) |
			      BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::AnalyticCubeSurvivors));

		if(JoinFactKindMaskFullyPassing(KindMask)) {
			FillPassBitsAllRows(Col);
			if(WantCube)
				BuildJoinFactPassBitsParallel(Col, KindMask, FkModA, FkModB, true);
			else
				BuildBulkSyntheticPassSparseWords(Col);
			return;
		}

		BuildJoinFactPassBitsParallel(Col, KindMask, FkModA, FkModB, WantCube);

		return;

	}



	if(Family == BulkSyntheticPassFamily::EntityScan) {
		const std::uint64_t Restrictive = KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractEq);
		if(Restrictive == 0 && KindMask != 0) {
			FillPassBitsAllRows(Col);
			BuildBulkSyntheticPassSparseWords(Col);
			return;
		}
		BuildEntityScanPassBitsParallel(Col, KindMask);
		return;
	}

	for(std::size_t Oi = 0; Oi < Col.RowCount; ++Oi) {

		const int64_t RowId = BulkSyntheticRowIdAt(Col, Oi);

		if(!SyntheticRowPassesPredicateMask(Family, RowId, RowId, KindMask, PrimaryJson, false, ReviewText))

			continue;

		const std::size_t Word = Oi >> 6;

		const std::size_t Bit = Oi & 63;

		Col.BulkSyntheticPassBits[Word] |= 1ULL << Bit;

		++Col.BulkSyntheticPassGroupCounts[Oi / kColumnRowGroupSize];

	}

	BuildBulkSyntheticPassSparseWords(Col);
}



void BuildBulkSyntheticPassSparseWords(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticPassBits.empty())
		return;
	const std::size_t Words = Col.BulkSyntheticPassBits.size();
	Col.BulkSyntheticPassSparseWords.clear();
	Col.BulkSyntheticPassSparseWords.reserve(Words / 8 + 1);
	for(std::size_t W = 0; W < Words; ++W) {
		if(Col.BulkSyntheticPassBits[W] != 0)
			Col.BulkSyntheticPassSparseWords.push_back(static_cast<std::uint32_t>(W));
	}
}

void EnsureBulkSyntheticPassSparseWords(ColumnarTable &Col) noexcept {
	if(!Col.BulkSyntheticPassSparseWords.empty() || Col.BulkSyntheticPassBits.empty())
		return;
	BuildBulkSyntheticPassSparseWords(Col);
}

bool BulkSyntheticPassBitsCoverQuery(const ColumnarTable &Col, const std::uint64_t QueryKindMask) noexcept {

	return PassBitsCoverQuery(Col, QueryKindMask);

}



std::uint64_t BulkSyntheticCountPassBits(const ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticMetadataOnly && Col.BulkSyntheticPassBits.empty())
		return DerivePassCount(Col);
	if(Col.BulkSyntheticPassBits.empty())
		return 0;
	if(!Col.BulkSyntheticPassGroupCounts.empty()) {
		std::uint64_t Total = 0;
		for(const std::uint32_t C : Col.BulkSyntheticPassGroupCounts)
			Total += static_cast<std::uint64_t>(C);
		return Total;
	}
	if(!Col.BulkSyntheticPassSparseWords.empty()) {
		std::uint64_t Total = 0;
		for(const std::uint32_t W : Col.BulkSyntheticPassSparseWords)
			Total += static_cast<std::uint64_t>(std::popcount(Col.BulkSyntheticPassBits[W]));
		return Total;
	}
	std::uint64_t Total = 0;
	for(const std::uint64_t Word : Col.BulkSyntheticPassBits)
		Total += static_cast<std::uint64_t>(std::popcount(Word));
	return Total;
}



bool BulkSyntheticJoinFactReady(const ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticPassFamilyTag != BulkSyntheticPassFamily::JoinFact || Col.BulkSyntheticFkCustMod <= 0 ||
	   Col.BulkSyntheticFkProdMod <= 0)
		return false;
	if(!Col.BulkSyntheticPassBits.empty())
		return true;
	return Col.BulkSyntheticMetadataOnly && Col.BulkSyntheticLazy && Col.RowCount > 0;
}

bool BulkSyntheticFusedJoinAggReady(const ColumnarTable &Col) noexcept {
	if(!BulkSyntheticJoinFactReady(Col) || Col.BulkSyntheticFkCustMod > 256 || Col.BulkSyntheticFkProdMod > 256)
		return false;
	if(Col.BulkSyntheticJoinGroupSlotByRow.size() == Col.RowCount &&
	   Col.BulkSyntheticAmountByRow.size() == Col.RowCount && !Col.BulkSyntheticJoinGroupSlotByRow.empty())
		return true;
	return Col.BulkSyntheticPassSlots.size() == Col.BulkSyntheticPassAmounts.size() &&
	       !Col.BulkSyntheticPassSlots.empty();
}



bool BulkSyntheticPassBitAt(const ColumnarTable &Col, const std::size_t RowIndex) noexcept {

	if(Col.BulkSyntheticPassBits.empty() || RowIndex >= Col.RowCount)

		return false;

	const std::size_t Word = RowIndex >> 6;

	const std::size_t Bit = RowIndex & 63;

	return (Col.BulkSyntheticPassBits[Word] & (1ULL << Bit)) != 0;

}



bool BulkSyntheticPassBitsApplicable(const BulkWhereDnf *Filters, const ColumnarTable &Col,
                                     const std::vector<Database::Column> &PrimarySchema,
                                     const std::vector<Database::Column> *LinkedSchema,
                                     const std::uint64_t QueryKindMask) noexcept {

	return PassBitsApplicable(Filters, Col, PrimarySchema, LinkedSchema, QueryKindMask);

}



} // namespace AstralDB

