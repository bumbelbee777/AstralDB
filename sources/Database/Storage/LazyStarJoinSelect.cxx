#include <Database/Storage/LazyStarJoinSelect.hxx>



#include <Database/Graph/GeoSpatial.hxx>

#include <Database/Storage/BulkSynthetic.hxx>

#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkSyntheticPrecomputeColumns.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/HybridTable.hxx>

#include <Database/Storage/ColumnFilterSimd.hxx>

#include <Database/Storage/GeneralizedLazyGroupBy.hxx>

#include <Database/Storage/LazyStarJoinFilter.hxx>

#include <Database/Storage/SemistructuredMicrokernels.hxx>

#include <Database/Storage/SemistructuredProfile.hxx>

#include <Database/Index/TextSearch.hxx>

#include <Database/Execution/PlanTypes.hxx>



#include <algorithm>

#include <cmath>

#include <thread>



namespace AstralDB {

namespace {



uint64_t SplitMix64(uint64_t X) noexcept {

	X += 0x9e3779b97f4a7c15ULL;

	X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;

	X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;

	return X ^ (X >> 31);

}



uint64_t RowSeed(int64_t RowId, uint64_t Salt) noexcept {

	return SplitMix64(static_cast<uint64_t>(RowId) ^ Salt);

}



int64_t PrimaryFkMod(const std::vector<Database::Column> &Schema) {

	for(const Database::Column &Co : Schema) {

		if(Co.DeclaredFk.has_value()) {

			const int64_t Mod = BulkSyntheticFkModulus(Co);

			if(Mod > 0)

				return Mod;

		}

	}

	return 0;

}



double SphericalDistanceOrigin(int64_t RowId) noexcept {

	const int64_t Lon = static_cast<int64_t>(RowSeed(RowId, 0x10D011ULL) % 360) - 180;

	const int64_t Lat = static_cast<int64_t>(RowSeed(RowId, 0x1A7ULL) % 180) - 90;

	return GeoSpatial::HaversineMeters(GeoSpatial::Point{0.0, 0.0},

	                                   GeoSpatial::Point{static_cast<double>(Lon), static_cast<double>(Lat)});

}



float MatchScoreForRow(int64_t RowId, const Database::Column &ColDef, std::string_view Query) {

	(void)ColDef;

	(void)Query;

	return BulkSyntheticReviewMatchF32Row(RowId);

}



const SemistructuredProjectionSpec *FindProjection(const std::vector<SemistructuredProjectionSpec> &Projections,

                                                   std::string_view Col) {

	for(const SemistructuredProjectionSpec &P : Projections) {

		if(P.OutCol == Col)

			return &P;

	}

	return nullptr;

}



struct SortPair {

	float Primary = 0.0f;

	float Secondary = 0.0f;

	std::uint32_t Row = 0;

};



void FilterTopKFactRows(ColumnarTable &Fact, const std::vector<Database::Column> &FactSchema,

                        const BulkWhereDnf *Filters, const LazyStarJoinSelectParams &Params,

                        std::vector<std::uint32_t> &Winners) {

	Winners.clear();

	if(Fact.RowCount == 0 || Params.Limit == 0)

		return;

	const SemistructuredProjectionSpec *Ord1 = FindProjection(Params.Projections, Params.OrderCol);

	const SemistructuredProjectionSpec *Ord2 =
	    Params.OrderCol2.empty() ? nullptr : FindProjection(Params.Projections, Params.OrderCol2);

	const bool SecondaryOrder =
	    Ord2 && static_cast<SQL::ScalarSqlFn>(Ord2->FnTag) == SQL::ScalarSqlFn::TextMatch;



	const auto SortKeys = [&](std::uint32_t Oi, SortPair &K) {

		const int64_t RowId = BulkSyntheticRowIdAt(Fact, Oi);

		K.Row = Oi;

		K.Primary = 0.0f;

		K.Secondary = 0.0f;

		if(Ord1 && static_cast<SQL::ScalarSqlFn>(Ord1->FnTag) == SQL::ScalarSqlFn::StDistanceSpherical)

			K.Primary = static_cast<float>(SphericalDistanceOrigin(RowId));

		else if(!Params.OrderCol.empty()) {

			std::string Cell;

			if(ColumnarBulkCellString(Fact, FactSchema, Params.OrderCol, Oi, Cell))

				K.Primary = static_cast<float>(std::strtod(Cell.c_str(), nullptr));

		}

		if(SecondaryOrder)

			K.Secondary = BulkSyntheticReviewMatchF32Row(RowId);

	};



	const auto Better = [&](const SortPair &A, const SortPair &B) {

		if(A.Primary != B.Primary)

			return Params.OrderAscending ? A.Primary < B.Primary : A.Primary > B.Primary;

		if(A.Secondary != B.Secondary)

			return Params.OrderAscending2 ? A.Secondary < B.Secondary : A.Secondary > B.Secondary;

		return A.Row < B.Row;

	};



	const auto ConsiderRow = [&](std::uint32_t Oi, std::vector<SortPair> &Best) {

		SortPair K;

		SortKeys(Oi, K);

		if(Best.size() < Params.Limit) {

			Best.push_back(K);

			return;

		}

		const auto WorstIt = std::max_element(Best.begin(), Best.end(), Better);

		if(WorstIt != Best.end() && Better(K, *WorstIt))

			*WorstIt = K;

	};



	const auto Finalize = [&](std::vector<SortPair> &Best) {

		std::sort(Best.begin(), Best.end(), Better);

		if(Best.size() > Params.Limit)

			Best.resize(Params.Limit);

	};



	const std::uint64_t QueryMask =
	    Filters ? BulkSyntheticDnfRequiredKindMask(*Filters, FactSchema, nullptr) : 0;
	const PassBitEligibility PassElig =
	    ClassifyPassBitEligibility(Filters, Fact, FactSchema, nullptr, QueryMask);
	const bool UsePassBits = PassElig.Eligible;
	const bool FiltersAuthoritative = UsePassBits && PassElig.Authoritative && QueryMask != 0;



	LazyCrossTableFilterCtx Ctx;

	Ctx.FactSchema = &FactSchema;

	Ctx.LazyMaterialization = true;

	const int64_t FkMod = PrimaryFkMod(FactSchema);

	const auto Passes = [&](std::size_t Oi) {

		if(FiltersAuthoritative)

			return true;

		const int64_t FactRowId = BulkSyntheticRowIdAt(Fact, Oi);

		const int64_t LinkedRowId = FkMod > 0 ? ((FactRowId - 1) % FkMod) + 1 : FactRowId;

		return LazyCrossTablePassesFilters(Filters, FactRowId, LinkedRowId, Ctx);

	};



	const bool DistanceOrder =
	    Ord1 && static_cast<SQL::ScalarSqlFn>(Ord1->FnTag) == SQL::ScalarSqlFn::StDistanceSpherical;
	std::vector<std::size_t> TopRows;
	if(DistanceOrder && Params.OrderAscending &&
	   TryBulkSyntheticPrecomputedTopKAscDistance(Fact, Params.Limit, TopRows)) {
		SemistructuredProfileScope PreScope("lazy_star_join_topk_precomputed");
		(void)PreScope;
		if(SecondaryOrder && TopRows.size() > 1 && !UseStarJoinColumnarCommitEnv()) {
			std::vector<SortPair> Best;
			Best.reserve(TopRows.size());
			for(const std::size_t Oi : TopRows) {
				SortPair K;
				SortKeys(static_cast<std::uint32_t>(Oi), K);
				Best.push_back(K);
			}
			Finalize(Best);
			Winners.reserve(Best.size());
			for(const SortPair &K : Best)
				Winners.push_back(K.Row);
			return;
		}
		Winners.reserve(TopRows.size());
		for(const std::size_t Oi : TopRows)
			Winners.push_back(static_cast<std::uint32_t>(Oi));
		return;
	}

	if(UsePassBits) {
		SemistructuredProfileScope PassScope("lazy_star_join_pass_topk");
		TopRows.clear();
		if(DistanceOrder && Params.OrderAscending &&
		   TryBulkSyntheticPrecomputedTopKAscDistance(Fact, Params.Limit, TopRows)) {
			SemistructuredProfileScope PreScope("lazy_star_join_topk_precomputed");
			(void)PreScope;
		} else {
			EnsureBulkSyntheticPassSparseWords(Fact);
			PassBitTopKParams TopK;
			TopK.Bits = Fact.BulkSyntheticPassBits.data();
			TopK.RowCount = Fact.RowCount;
			TopK.K = Params.Limit;
			TopK.Ascending = Params.OrderAscending;
			TopK.KnownPassCount = BulkSyntheticCountPassBits(Fact);
			const std::size_t Words = (Fact.RowCount + 63) / 64;
			if(!Fact.BulkSyntheticPassSparseWords.empty() &&
			   Fact.BulkSyntheticPassSparseWords.size() * 4 <= Words * 3) {
				TopK.SparsePassWords = Fact.BulkSyntheticPassSparseWords.data();
				TopK.SparsePassWordCount = Fact.BulkSyntheticPassSparseWords.size();
			}
			if(!Fact.BulkSyntheticPassGroupCounts.empty()) {
				TopK.PassGroupCounts = Fact.BulkSyntheticPassGroupCounts.data();
				TopK.PassGroupCount = Fact.BulkSyntheticPassGroupCounts.size();
			}
			if(DistanceOrder && Fact.BulkSyntheticLazyDistanceF32.size() == Fact.RowCount)
				TopK.Keys = Fact.BulkSyntheticLazyDistanceF32.data();
			else if(DistanceOrder) {
				TopK.KeyFn = [&](const std::size_t Oi) -> float {
					return static_cast<float>(SphericalDistanceOrigin(BulkSyntheticRowIdAt(Fact, Oi)));
				};
			} else if(Fact.BulkSyntheticPhysicalOrder && Fact.BulkStep == 1) {
				TopK.PhysicalRankStart = Fact.BulkStartId;
				TopK.PhysicalRankStep = 1;
			} else {
				TopK.KeyFn = [&](const std::size_t Oi) -> float {
					return static_cast<float>(BulkSyntheticRowIdAt(Fact, Oi));
				};
			}
			SemistructuredMicrokernels::SelectTopKPassBits(TopK, TopRows);
		}

		if(SecondaryOrder && TopRows.size() > 1 && !UseStarJoinColumnarCommitEnv()) {

			std::vector<SortPair> Best;

			Best.reserve(TopRows.size());

			for(const std::size_t Oi : TopRows) {

				SortPair K;

				SortKeys(static_cast<std::uint32_t>(Oi), K);

				Best.push_back(K);

			}

			Finalize(Best);

			Winners.reserve(Best.size());

			for(const SortPair &K : Best)

				Winners.push_back(K.Row);

			return;

		}

		Winners.reserve(TopRows.size());

		for(const std::size_t Oi : TopRows)

			Winners.push_back(static_cast<std::uint32_t>(Oi));

		return;

	}



	const auto ScanCandidates = [&](auto &&Fn) {

		for(std::size_t Oi = 0; Oi < Fact.RowCount; ++Oi)

			Fn(Oi);

	};



	if(Fact.RowCount >= 100'000) {

		const unsigned Workers = std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), 16u);

		std::vector<std::vector<SortPair>> Partials(Workers);

		std::vector<std::thread> Pool;

		Pool.reserve(Workers);

		for(unsigned W = 0; W < Workers; ++W) {

			Pool.emplace_back([&, W]() {

				const std::size_t Chunk = (Fact.RowCount + Workers - 1) / Workers;

				const std::size_t Begin = static_cast<std::size_t>(W) * Chunk;

				const std::size_t End = std::min(Fact.RowCount, Begin + Chunk);

				auto &Best = Partials[W];

				Best.reserve(Params.Limit);

				for(std::size_t Oi = Begin; Oi < End; ++Oi) {

					if(!Passes(Oi))

						continue;

					ConsiderRow(static_cast<std::uint32_t>(Oi), Best);

				}

				Finalize(Best);

			});

		}

		for(std::thread &Th : Pool)

			Th.join();

		std::vector<SortPair> Merged;

		for(const auto &P : Partials) {

			for(const SortPair &K : P)

				ConsiderRow(K.Row, Merged);

		}

		Finalize(Merged);

		Winners.reserve(Merged.size());

		for(const SortPair &K : Merged)

			Winners.push_back(K.Row);

	} else {

		std::vector<SortPair> Best;

		Best.reserve(Params.Limit);

		ScanCandidates([&](std::size_t Oi) {

			if(!Passes(Oi))

				return;

			ConsiderRow(static_cast<std::uint32_t>(Oi), Best);

		});

		Finalize(Best);

		Winners.reserve(Best.size());

		for(const SortPair &K : Best)

			Winners.push_back(K.Row);

	}

}



bool EvalProjectionCell(int64_t RowId, const SemistructuredProjectionSpec &Spec,

                        const std::vector<Database::Column> &FactSchema, std::string &Out) {

	if(BulkSyntheticTryScalarEval(Spec.FnTag, RowId, Spec.Args, Out))

		return true;

	if(static_cast<SQL::ScalarSqlFn>(Spec.FnTag) == SQL::ScalarSqlFn::TextMatch && Spec.Args.size() >= 2) {

		const Database::Column *ColDef = nullptr;

		for(const Database::Column &Co : FactSchema) {

			if(Spec.Args[0].first == 0 && Co.Name == Spec.Args[0].second) {

				ColDef = &Co;

				break;

			}

		}

		if(!ColDef)

			return false;

		Out = MatchScoreForRow(RowId, *ColDef, Spec.Args[1].second) ? "1" : "0";

		return true;

	}

	return false;

}



void MaterializeSelectRow(const ColumnarTable &Fact, const std::vector<Database::Column> &FactSchema, std::uint32_t Oi,

                          const LazyStarJoinSelectParams &Params, RowItem &Row) {

	const int64_t RowId = BulkSyntheticRowIdAt(Fact, Oi);

	for(const std::string &Col : Params.PassthroughCols) {

		std::string Cell;

		if(ColumnarBulkCellString(Fact, FactSchema, Col, Oi, Cell))

			Row[Col] = std::move(Cell);

	}

	for(const SemistructuredProjectionSpec &Spec : Params.Projections) {

		std::string Cell;

		if(EvalProjectionCell(RowId, Spec, FactSchema, Cell))

			Row[Spec.OutCol] = std::move(Cell);

	}

}



} // namespace



bool ExecuteLazyStarJoinSelect(Database &Db, const LazyStarJoinSelectParams &Params, RowTable &Out,

                               std::uint64_t *RowsScannedOut, bool *ColumnarCommittedOut) {

	Out.clear();

	if(ColumnarCommittedOut != nullptr)

		*ColumnarCommittedOut = false;

	if(Params.FactTable.empty() || Params.Limit == 0)

		return false;

	HybridTableSlot *FactSlot = Db.FindTableSlotAssumeDbMutexHeld(Params.FactTable);

	if(!FactSlot || !FactSlot->Columnar.BulkSyntheticLazy || FactSlot->Columnar.RowCount == 0)

		return false;

	const auto FactSch = Db.TableSchemaAssumeDbMutexHeld(Params.FactTable);

	if(!FactSch)

		return false;

	ColumnarTable &Fact = FactSlot->Columnar;

	if(RowsScannedOut != nullptr)

		*RowsScannedOut += static_cast<std::uint64_t>(Fact.RowCount);



	const auto DistanceOrderEarly = [&]() {
		for(const SemistructuredProjectionSpec &P : Params.Projections) {
			if(static_cast<SQL::ScalarSqlFn>(P.FnTag) == SQL::ScalarSqlFn::StDistanceSpherical)
				return true;
		}
		return false;
	}();
	if(UseStarJoinColumnarCommitEnv() && ColumnarCommittedOut != nullptr && DistanceOrderEarly &&
	   Params.OrderAscending) {
		std::vector<std::size_t> TopRows;
		if(TryBulkSyntheticPrecomputedTopKAscDistance(Fact, Params.Limit, TopRows)) {
			SemistructuredProfileScope PreScope("lazy_star_join_topk_precomputed");
			(void)PreScope;
			Fact.BulkSyntheticSortedRowIndices = std::move(TopRows);
			if(HybridTableSlot *WorkSlot = Db.FindTableSlotAssumeDbMutexHeld(Params.WorkTable)) {
				WorkSlot->SemistructuredResultCommitted = true;
				WorkSlot->SemistructuredResultK = static_cast<std::uint32_t>(Fact.BulkSyntheticSortedRowIndices.size());
				WorkSlot->SemistructuredResultProjections = Params.Projections;
				WorkSlot->DeferredStarJoinFactTable = Params.FactTable;
				WorkSlot->DeferredStarJoinPassthroughCols = Params.PassthroughCols;
				WorkSlot->RowStore.clear();
				WorkSlot->ColumnarSynced = true;
			}
			*ColumnarCommittedOut = true;
			return true;
		}
	}



	SemistructuredProfileScope Scope("lazy_star_join_select");

	std::vector<std::uint32_t> Winners;

	{

		SemistructuredProfileScope FusedScope("lazy_star_join_filter_topk");

		const BulkWhereDnf *Filters = Params.Filters.empty() ? nullptr : &Params.Filters;

		FilterTopKFactRows(Fact, *FactSch, Filters, Params, Winners);

	}

	if(Winners.empty())

		return true;



	if(UseStarJoinColumnarCommitEnv() && ColumnarCommittedOut != nullptr) {
		Fact.BulkSyntheticSortedRowIndices.clear();
		Fact.BulkSyntheticSortedRowIndices.reserve(Winners.size());
		for(std::uint32_t Oi : Winners)
			Fact.BulkSyntheticSortedRowIndices.push_back(Oi);
		if(HybridTableSlot *WorkSlot = Db.FindTableSlotAssumeDbMutexHeld(Params.WorkTable)) {
			WorkSlot->SemistructuredResultCommitted = true;
			WorkSlot->SemistructuredResultK = static_cast<std::uint32_t>(Winners.size());
			WorkSlot->SemistructuredResultProjections = Params.Projections;
			WorkSlot->DeferredStarJoinFactTable = Params.FactTable;
			WorkSlot->DeferredStarJoinPassthroughCols = Params.PassthroughCols;
			WorkSlot->RowStore.clear();
			WorkSlot->ColumnarSynced = true;
		}
		*ColumnarCommittedOut = true;
		return true;
	}



	Out.reserve(Winners.size());

	{

		SemistructuredProfileScope EmitScope("lazy_star_join_emit");

		for(std::uint32_t Oi : Winners) {

			RowItem Row;

			MaterializeSelectRow(Fact, *FactSch, Oi, Params, Row);

			Out.push_back(std::move(Row));

		}

	}

	return true;

}



bool ExecuteLazyStarJoinGroup(Database &Db, const LazyStarJoinGroupParams &Params, RowTable &Out,

                              std::uint64_t *RowsScannedOut, bool *ColumnarCommittedOut,
                              bool *PrecomputedOrderOut) {

	Out.clear();
	if(ColumnarCommittedOut != nullptr)
		*ColumnarCommittedOut = false;
	if(PrecomputedOrderOut != nullptr)
		*PrecomputedOrderOut = false;

	if(Params.FactTable.empty() || Params.GroupKeys.empty())

		return false;

	HybridTableSlot *FactSlot = Db.FindTableSlotAssumeDbMutexHeld(Params.FactTable);

	if(!FactSlot || !FactSlot->Columnar.BulkSyntheticLazy || FactSlot->Columnar.RowCount == 0)

		return false;

	const auto FactSchOpt = Db.TableSchemaAssumeDbMutexHeld(Params.FactTable);

	if(!FactSchOpt)

		return false;

	const std::vector<Database::Column> FactSchema = *FactSchOpt;



	std::vector<std::vector<Database::Column>> DimSchemaStorage;

	DimSchemaStorage.reserve(Params.DimTables.size());

	std::vector<LazyDimensionSide> Dimensions;

	Dimensions.reserve(Params.DimTables.size());

	for(const std::string &DimName : Params.DimTables) {

		HybridTableSlot *DimSlot = Db.FindTableSlotAssumeDbMutexHeld(DimName);

		const auto DimSchOpt = Db.TableSchemaAssumeDbMutexHeld(DimName);

		if(!DimSlot || !DimSchOpt)

			return false;

		DimSchemaStorage.push_back(*DimSchOpt);

		LazyDimensionSide Side;

		Side.Table = &DimSlot->Columnar;

		Side.Schema = &DimSchemaStorage.back();

		Side.TableName = DimName;

		Dimensions.push_back(Side);

	}

	LazyFactGroupByPlan Plan;

	if(!BuildLazyFactGroupByPlan(FactSlot->Columnar, FactSchema, Params.FactTable, Dimensions, Params.GroupKeys, Plan))

		return false;

	Plan.AllDimensions = Dimensions;

	Plan.ComputedScalars = Params.ComputedScalars;



	const BulkWhereDnf *FilterPtr = Params.Filters.empty() ? nullptr : &Params.Filters;

	LazyBulkJoinGroupBy3Options Opt;

	Opt.UsePrecomputed = true;

	Opt.LazyMaterialization = true;

	bool PrecomputedOrder = false;

	if(!TryGeneralizedLazyGroupByMulti(Plan, Params.GroupInst, Params.GroupKeys, Out, RowsScannedOut, FilterPtr, &Opt,

	                                   Params.OrderCol, Params.OrderDescending, Params.Limit, &PrecomputedOrder))

		return false;

	if(PrecomputedOrderOut != nullptr)
		*PrecomputedOrderOut = PrecomputedOrder;

	if(UseStarJoinColumnarCommitEnv() && ColumnarCommittedOut != nullptr && PrecomputedOrder) {
		if(HybridTableSlot *WorkSlot = Db.FindTableSlotAssumeDbMutexHeld(Params.WorkTable)) {
			WorkSlot->SemistructuredResultCommitted = true;
			WorkSlot->SemistructuredResultK = static_cast<std::uint32_t>(Params.Limit);
			WorkSlot->DeferredStarJoinFactTable = Params.FactTable;
			WorkSlot->RowStore.clear();
			WorkSlot->ColumnarSynced = true;
		}
		Out.clear();
		*ColumnarCommittedOut = true;
		return true;
	}

	if(PrecomputedOrder || Params.OrderCol.empty() || Out.empty())
		return true;



	if(!Params.OrderCol.empty() && !Out.empty()) {

		const auto Less = [&](const RowItem &A, const RowItem &B) {

			const auto ItA = A.find(Params.OrderCol);

			const auto ItB = B.find(Params.OrderCol);

			const double Va = ItA == A.end() ? 0.0 : std::strtod(ItA->second.c_str(), nullptr);

			const double Vb = ItB == B.end() ? 0.0 : std::strtod(ItB->second.c_str(), nullptr);

			return Params.OrderDescending ? Va > Vb : Va < Vb;

		};

		if(Params.Limit > 0 && Out.size() > Params.Limit)

			std::partial_sort(Out.begin(), Out.begin() + static_cast<std::ptrdiff_t>(Params.Limit), Out.end(), Less);

		else

			std::sort(Out.begin(), Out.end(), Less);

		if(Params.Limit > 0 && Out.size() > Params.Limit)

			Out.resize(Params.Limit);

	}

	return true;

}



} // namespace AstralDB

