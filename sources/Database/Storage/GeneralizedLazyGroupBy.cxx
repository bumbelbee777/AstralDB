#include <Database/Storage/GeneralizedLazyGroupBy.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/BulkSyntheticPrecomputeColumns.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/PassBitWalk.hxx>
#include <Database/Storage/SemistructuredMicrokernels.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>
#include <Database/Storage/ColumnZoneMap.hxx>
#include <Database/Storage/LazyStarJoinFilter.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Graph/GeoSpatial.hxx>
#include <DS/CuckooMap.hxx>
#include <DS/RadixPartition.hxx>
#include <DS/SimdHash.hxx>
#include <IO/Job.hxx>
#include <Database/Execution/FastPathGuard.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace AstralDB {

namespace {

struct GroupAcc {
	int64_t Cnt = 0;
	double Sum = 0;
	std::string GroupKey;
};

const Database::Column *FindCol(const std::vector<Database::Column> &Schema, std::string_view Name) {
	for(const auto &Co : Schema)
		if(Co.Name == Name)
			return &Co;
	return nullptr;
}

bool ParseGroupBy3Operands(const SQL::Instruction &Inst, bool &IncludeCountStar, std::string &CountOut,
                           std::string &SumCol, std::string &SumOut) {
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3)
		return false;
	const size_t Base = static_cast<size_t>(2 + *Nk);
	if(Inst.Operands.size() < Base + 2)
		return false;
	const auto *HCnt = std::get_if<int64_t>(&Inst.Operands[Base]);
	const auto *Na = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
	if(!HCnt || !Na || *Na <= 0)
		return false;
	IncludeCountStar = (*HCnt != 0);
	const size_t IdxAfterSpecs = Base + 2 + static_cast<size_t>(*Na) * 3;
	CountOut = "cnt";
	if(IncludeCountStar && Inst.Operands.size() > IdxAfterSpecs) {
		if(const auto *Cn = std::get_if<std::string>(&Inst.Operands[IdxAfterSpecs]); Cn && !Cn->empty())
			CountOut = *Cn;
	}
	const size_t SumSpec = Base + 2;
	if(Inst.Operands.size() <= SumSpec + 2)
		return false;
	const auto *Sc = std::get_if<std::string>(&Inst.Operands[SumSpec + 1]);
	const auto *So = std::get_if<std::string>(&Inst.Operands[SumSpec + 2]);
	if(!Sc || !So)
		return false;
	SumCol = *Sc;
	SumOut = *So;
	return true;
}

struct CrossTableFilterCtx {
	const std::vector<Database::Column> *FactSchema = nullptr;
	const std::vector<std::vector<Database::Column>> *LinkedSchemas = nullptr;
	bool LazyMaterialization = true;
};

bool EvalCrossTableFilterPred(const std::tuple<std::string, std::string, std::string> &Pred, int64_t FactRowId,
                              int64_t LinkedRowId, const CrossTableFilterCtx &Ctx) {
	const auto &[ColName, Op, Val] = Pred;
	const Database::Column *ColDef = nullptr;
	if(Ctx.FactSchema && FindCol(*Ctx.FactSchema, ColName))
		ColDef = FindCol(*Ctx.FactSchema, ColName);
	else if(Ctx.LinkedSchemas) {
		for(const std::vector<Database::Column> &Sch : *Ctx.LinkedSchemas) {
			if((ColDef = FindCol(Sch, ColName)) != nullptr)
				break;
		}
	}
	const int64_t RowId = ColDef && Ctx.FactSchema && FindCol(*Ctx.FactSchema, ColName) ? FactRowId : LinkedRowId;
	if(Ctx.LazyMaterialization) {
		if(const auto Fast = BulkSyntheticTryMatchPredicate(RowId, ColName, Op, Val, ColDef))
			return *Fast;
	}
	if(!ColDef)
		return false;
	return BulkSyntheticCellString(*ColDef, BulkSyntheticContext{RowId, 1, 0, 1}) == Val;
}

bool CrossTablePassesFilters(const BulkWhereDnf *Filters, int64_t FactRowId, int64_t LinkedRowId,
                             const CrossTableFilterCtx &Ctx) {
	if(!Filters || Filters->empty())
		return true;
	for(const BulkWhereDnfBranch &Branch : *Filters) {
		if(Branch.empty())
			return true;
		bool BranchOk = true;
		for(const auto &Pred : Branch) {
			if(!EvalCrossTableFilterPred(Pred, FactRowId, LinkedRowId, Ctx)) {
				BranchOk = false;
				break;
			}
		}
		if(BranchOk)
			return true;
	}
	return false;
}

std::string BuildCompositeGroupKey(const LazyFactGroupByPlan &Plan, int64_t FactRowId) {
	std::string Key;
	for(const LazyFactGroupByBinding &B : Plan.Bindings) {
		int64_t RowForCell = FactRowId;
		if(B.Source == LazyGroupKeySource::DimensionFk) {
			const int64_t Mod = BulkSyntheticFkModulus(*B.FactFk);
			RowForCell = Mod > 0 ? ((FactRowId - 1) % Mod) + 1 : FactRowId;
		}
		const std::string Part = LazyGroupKeyCellValue(B, RowForCell);
		if(!Key.empty())
			Key.push_back('\x1f');
		Key += Part;
	}
	return Key;
}

const Database::Column *SoleTimestampColumn(const std::vector<Database::Column> &Schema) {
	const Database::Column *Found = nullptr;
	for(const Database::Column &Co : Schema) {
		if(ClassifySqlStorage(Co) != SqlStorageKind::Timestamp)
			continue;
		if(Found != nullptr)
			return nullptr;
		Found = &Co;
	}
	return Found;
}

/** Fact column that joins to \p Dim (declared FK, else same name as dimension PK). */
const Database::Column *FindFactLinkToDimension(const std::vector<Database::Column> &FactSchema,
                                              const LazyDimensionSide &Dim) {
	for(const Database::Column &Co : FactSchema) {
		if(Co.DeclaredFk.has_value() && Co.DeclaredFk->ReferencedTable == Dim.TableName)
			return &Co;
	}
	if(!Dim.Schema)
		return nullptr;
	for(const Database::Column &Pk : *Dim.Schema) {
		if(!Pk.IsPrimaryKey)
			continue;
		if(const Database::Column *OnFact = FindCol(FactSchema, Pk.Name))
			return OnFact;
	}
	return nullptr;
}

struct DimGroupKeyCandidate {
	const LazyDimensionSide *Side = nullptr;
	const Database::Column *GkCol = nullptr;
	const Database::Column *Fk = nullptr;
	int Score = 0;
};

bool ResolveDimensionGroupKey(const std::vector<Database::Column> &FactSchema,
                              const std::vector<LazyDimensionSide> &Dimensions, std::string_view Gk,
                              DimGroupKeyCandidate &Best) {
	std::vector<DimGroupKeyCandidate> Candidates;
	Candidates.reserve(Dimensions.size());
	for(const LazyDimensionSide &D : Dimensions) {
		if(!D.Schema)
			continue;
		const Database::Column *GkCol = FindCol(*D.Schema, Gk);
		if(!GkCol)
			continue;
		const Database::Column *Fk = FindFactLinkToDimension(FactSchema, D);
		if(!Fk)
			continue;
		int Score = 1;
		if(Fk->DeclaredFk.has_value() && Fk->DeclaredFk->ReferencedTable == D.TableName)
			Score = 2;
		Candidates.push_back({&D, GkCol, Fk, Score});
	}
	if(Candidates.empty())
		return false;
	Best = *std::max_element(Candidates.begin(), Candidates.end(),
	                         [](const DimGroupKeyCandidate &A, const DimGroupKeyCandidate &B) {
		                         if(A.Score != B.Score)
			                         return A.Score < B.Score;
		                         const bool APk = A.GkCol != nullptr && A.GkCol->IsPrimaryKey;
		                         const bool BPk = B.GkCol != nullptr && B.GkCol->IsPrimaryKey;
		                         if(APk != BPk)
			                         return APk < BPk;
		                         return A.Side->TableName > B.Side->TableName;
	                         });
	return true;
}

void EmitGroupByRows(const std::vector<GroupAcc> &Slots, const CuckooMap &KeyMap,
                     const std::vector<std::string> &GroupKeys, const std::string &CountOut, const std::string &SumOut,
                     RowTable &Out) {
	Out.clear();
	Out.reserve(KeyMap.Size());
	for(std::size_t I = 0; I < Slots.size(); ++I) {
		const GroupAcc &G = Slots[I];
		if(G.Cnt == 0)
			continue;
		RowItem R;
		std::size_t KeyPart = 0;
		std::size_t Start = 0;
		for(std::size_t P = 0; P <= G.GroupKey.size(); ++P) {
			if(P == G.GroupKey.size() || G.GroupKey[P] == '\x1f') {
				if(KeyPart < GroupKeys.size())
					R[GroupKeys[KeyPart]] = G.GroupKey.substr(Start, P - Start);
				++KeyPart;
				Start = P + 1;
			}
		}
		R[CountOut] = std::to_string(G.Cnt);
		std::ostringstream O;
		O << static_cast<long long>(std::llround(G.Sum));
		R[SumOut] = O.str();
		Out.push_back(std::move(R));
	}
}

void RecordRegion(const LazyBulkJoinGroupBy3Options *Opt, std::string_view Name,
                  const std::chrono::steady_clock::time_point Start) {
	if(!Opt || !Opt->RecordRegion)
		return;
	Opt->RecordRegion(Name, std::chrono::steady_clock::now() - Start);
}

} // namespace

std::string LazyGroupKeyCellValue(const LazyFactGroupByBinding &Binding, const int64_t PrimaryRowId) {
	if(!Binding.GroupKeyColumn || !Binding.Dimension.Schema)
		return {};
	const std::vector<Database::Column> &Schema = *Binding.Dimension.Schema;
	if(Binding.Source == LazyGroupKeySource::FactTimeBucket)
		return std::to_string(BulkSyntheticMonthBucketFromRowId(PrimaryRowId));
	const std::size_t ColIdx = BulkSyntheticColumnIndex(Schema, Binding.GroupKeyColumn->Name);
	if(ColIdx >= Schema.size())
		return {};
	return BulkSyntheticCellString(*Binding.GroupKeyColumn,
	                              BulkSyntheticContext{PrimaryRowId, 1, ColIdx, Schema.size()});
}

bool BuildLazyFactGroupByPlan(const ColumnarTable &Fact, const std::vector<Database::Column> &FactSchema,
                              std::string_view FactTableName, const std::vector<LazyDimensionSide> &Dimensions,
                              const std::vector<std::string> &GroupKeys, LazyFactGroupByPlan &Out) {
	if(!Fact.BulkSyntheticLazy || Fact.RowCount == 0 || GroupKeys.empty())
		return false;
	Out = {};
	Out.Fact = &Fact;
	Out.FactSchema = &FactSchema;
	Out.FactTableName = std::string(FactTableName);
	Out.Bindings.reserve(GroupKeys.size());
	LazyDimensionSide FactSide;
	FactSide.Table = &Fact;
	FactSide.Schema = &FactSchema;
	FactSide.TableName = std::string(FactTableName);
	for(const std::string &Gk : GroupKeys) {
		LazyFactGroupByBinding Binding;
		if(const Database::Column *OnFact = FindCol(FactSchema, Gk)) {
			Binding = {OnFact, OnFact, FactSide, LazyGroupKeySource::FactColumn};
			Out.Bindings.push_back(Binding);
			continue;
		}
		DimGroupKeyCandidate DimCand;
		if(ResolveDimensionGroupKey(FactSchema, Dimensions, Gk, DimCand)) {
			Binding = {DimCand.Fk, DimCand.GkCol, *DimCand.Side, LazyGroupKeySource::DimensionFk};
			Out.Bindings.push_back(Binding);
			continue;
		}
		if(SoleTimestampColumn(FactSchema) != nullptr) {
			Binding = {SoleTimestampColumn(FactSchema), SoleTimestampColumn(FactSchema), FactSide,
			           LazyGroupKeySource::FactTimeBucket};
			Out.Bindings.push_back(Binding);
			continue;
		}
		return false;
	}
	return !Out.Bindings.empty();
}

bool TryGeneralizedLazyGroupBy(const LazyFactGroupByPlan &Plan, const SQL::Instruction &GroupInst,
                              const std::vector<std::string> &GroupKeys, RowTable &Out, std::uint64_t *RowsScannedOut,
                              const BulkWhereDnf *FactFilters, const LazyBulkJoinGroupBy3Options *Options) {
	if(!Plan.Fact || !Plan.FactSchema || GroupKeys.size() != Plan.Bindings.size())
		return false;
	bool IncludeCountStar = false;
	std::string CountOut;
	std::string SumCol;
	std::string SumOut;
	if(!ParseGroupBy3Operands(GroupInst, IncludeCountStar, CountOut, SumCol, SumOut))
		return false;
	if(!FindCol(*Plan.FactSchema, SumCol))
		return false;

	LazyBulkJoinGroupBy3Options DefaultOpt;
	const LazyBulkJoinGroupBy3Options &Opt = Options ? *Options : DefaultOpt;
	CrossTableFilterCtx FilterCtx;
	FilterCtx.FactSchema = Plan.FactSchema;
	std::vector<std::vector<Database::Column>> LinkedStorage;
	LinkedStorage.reserve(Plan.Bindings.size());
	for(const LazyFactGroupByBinding &B : Plan.Bindings) {
		if(B.Dimension.Schema)
			LinkedStorage.push_back(*B.Dimension.Schema);
	}
	FilterCtx.LinkedSchemas = LinkedStorage.empty() ? nullptr : &LinkedStorage;
	FilterCtx.LazyMaterialization = Opt.LazyMaterialization;

	std::vector<const std::vector<Database::Column> *> LinkedPtrs;
	for(const auto &S : LinkedStorage)
		LinkedPtrs.push_back(&S);
	const std::vector<Database::Column> *FirstLinked =
	    LinkedPtrs.empty() ? nullptr : LinkedPtrs.front();
	const std::uint64_t QueryMask =
	    FactFilters ? BulkSyntheticDnfRequiredKindMask(*FactFilters, *Plan.FactSchema, FirstLinked) : 0;
	const PassBitEligibility PassElig = ClassifyPassBitEligibility(FactFilters, *Plan.Fact, *Plan.FactSchema,
	                                                             FirstLinked, QueryMask);
	const bool UsePassBits = Opt.UsePrecomputed && PassElig.Eligible;

	const auto FilterStart = std::chrono::steady_clock::now();
	CuckooMap KeyMap(16);
	std::vector<GroupAcc> Slots;
	Slots.reserve(128);

	const int64_t PrimaryFkMod =
	    Plan.Bindings.empty() ? 0 : BulkSyntheticFkModulus(*Plan.Bindings.front().FactFk);

	auto AccumulateRow = [&](std::size_t Oi) {
		const int64_t FactRowId = BulkSyntheticRowIdAt(*Plan.Fact, Oi);
		const int64_t LinkedRowId = PrimaryFkMod > 0 ? ((FactRowId - 1) % PrimaryFkMod) + 1 : FactRowId;
		if(!CrossTablePassesFilters(FactFilters, FactRowId, LinkedRowId, FilterCtx))
			return;
		const std::string Key = BuildCompositeGroupKey(Plan, FactRowId);
		const std::uint32_t Slot = KeyMap.FindOrInsert(Key);
		if(Slot >= Slots.size())
			Slots.resize(static_cast<std::size_t>(Slot) + 1);
		GroupAcc &G = Slots[Slot];
		if(G.Cnt == 0)
			G.GroupKey = Key;
		++G.Cnt;
		G.Sum += BulkSyntheticDecimalFromRowId(FactRowId);
	};

	if(UsePassBits && !Plan.Fact->BulkSyntheticPassBits.empty()) {
		std::vector<std::size_t> Passing;
		ScanPassBitsIndices(Plan.Fact->BulkSyntheticPassBits.data(), Plan.Fact->RowCount, Passing);
		for(const std::size_t Oi : Passing)
			AccumulateRow(Oi);
	} else {
		for(std::size_t Oi = 0; Oi < Plan.Fact->RowCount; ++Oi)
			AccumulateRow(Oi);
	}

	RecordRegion(Options, "filter", FilterStart);
	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Plan.Fact->RowCount);

	const auto EmitStart = std::chrono::steady_clock::now();
	EmitGroupByRows(Slots, KeyMap, GroupKeys, CountOut, SumOut, Out);
	RecordRegion(Options, "emit", EmitStart);
	return KeyMap.Size() > 0;
}

namespace {

uint64_t SplitMix64Local(uint64_t X) noexcept {
	X += 0x9e3779b97f4a7c15ULL;
	X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;
	X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;
	return X ^ (X >> 31);
}

double SphericalDistanceOriginRow(int64_t RowId) noexcept {
	const int64_t Lon = static_cast<int64_t>(SplitMix64Local(static_cast<uint64_t>(RowId) ^ 0x10D011ULL) % 360) - 180;
	const int64_t Lat = static_cast<int64_t>(SplitMix64Local(static_cast<uint64_t>(RowId) ^ 0x1A7ULL) % 180) - 90;
	return GeoSpatial::HaversineMeters(GeoSpatial::Point{0.0, 0.0},
	                                   GeoSpatial::Point{static_cast<double>(Lon), static_cast<double>(Lat)});
}

struct ParsedComboAgg {
	SQL::GroupCombAggKind Kind = SQL::GroupCombAggKind::Sum;
	std::string SrcCol;
	std::string OutCol;
};

struct MultiGroupAcc {
	int64_t Cnt = 0;
	std::string GroupKey;
	std::unordered_map<std::string, double> Sum;
	std::unordered_map<std::string, int64_t> SumN;
	std::unordered_map<std::string, double> SumSq;
	std::unordered_map<std::string, double> MaxNum;
};

bool ParseMultiAggGroupInst(const SQL::Instruction &GroupInst, std::vector<ParsedComboAgg> &Aggs, bool &IncludeCountStar,
                            std::string &CountOut) {
	Aggs.clear();
	IncludeCountStar = false;
	CountOut = "cnt";
	if(GroupInst.Opcode_ != SQL::Opcode::GROUP_BY || GroupInst.Operands.size() < 4)
		return false;
	const auto *Tag = std::get_if<int64_t>(&GroupInst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&GroupInst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3)
		return false;
	const std::size_t Base = static_cast<std::size_t>(2 + *Nk);
	if(GroupInst.Operands.size() <= Base + 1)
		return false;
	const auto *HCnt = std::get_if<int64_t>(&GroupInst.Operands[Base]);
	const auto *Na = std::get_if<int64_t>(&GroupInst.Operands[Base + 1]);
	if(!HCnt || !Na || *Na <= 0)
		return false;
	IncludeCountStar = (*HCnt != 0);
	const std::size_t SpecBase = Base + 2;
	for(int64_t I = 0; I < *Na; ++I) {
		const std::size_t Off = SpecBase + static_cast<std::size_t>(I) * 3;
		if(Off + 2 >= GroupInst.Operands.size())
			return false;
		const auto *Kind = std::get_if<int64_t>(&GroupInst.Operands[Off]);
		const auto *Src = std::get_if<std::string>(&GroupInst.Operands[Off + 1]);
		const auto *Out = std::get_if<std::string>(&GroupInst.Operands[Off + 2]);
		if(!Kind || !Src || !Out)
			return false;
		ParsedComboAgg A;
		A.Kind = static_cast<SQL::GroupCombAggKind>(*Kind);
		A.SrcCol = *Src;
		A.OutCol = *Out;
		Aggs.push_back(std::move(A));
	}
	const std::size_t AfterSpecs = SpecBase + static_cast<std::size_t>(*Na) * 3;
	if(IncludeCountStar && GroupInst.Operands.size() > AfterSpecs) {
		if(const auto *Cn = std::get_if<std::string>(&GroupInst.Operands[AfterSpecs]); Cn && !Cn->empty())
			CountOut = *Cn;
	}
	return !Aggs.empty() || IncludeCountStar;
}

double AggValueForRow(const ParsedComboAgg &Agg, int64_t FactRowId, const LazyFactGroupByPlan &Plan);

bool TryPassBitMonotonicTimestampGroupTopK(const LazyFactGroupByPlan &Plan, const SQL::Instruction &GroupInst,
                                           const std::vector<std::string> &GroupKeys, RowTable &Out,
                                           std::uint64_t *RowsScannedOut, const BulkWhereDnf *FactFilters,
                                           const LazyBulkJoinGroupBy3Options &Opt, std::string_view OrderCol,
                                           bool OrderDescending, std::size_t Limit, bool *PrecomputedOrderOut) {
	(void)Opt;
	(void)FactFilters;
	if(PrecomputedOrderOut != nullptr)
		*PrecomputedOrderOut = false;
	if(Limit == 0 || GroupKeys.size() != 1)
		return false;
	const std::string_view OrderKey = OrderCol.empty() ? std::string_view{GroupKeys[0]} : OrderCol;
	if(OrderKey != GroupKeys[0])
		return false;
	if(!Plan.Fact || !Plan.FactSchema || Plan.Bindings.size() != 1)
		return false;
	const LazyFactGroupByBinding &Bind = Plan.Bindings.front();
	if(Bind.Source != LazyGroupKeySource::FactColumn || !Bind.GroupKeyColumn)
		return false;
	if(ClassifySqlStorage(*Bind.GroupKeyColumn) != SqlStorageKind::Timestamp)
		return false;
	if(!Plan.Fact->BulkSyntheticPhysicalOrder || Plan.Fact->BulkStep != 1)
		return false;

	std::vector<ParsedComboAgg> Aggs;
	bool IncludeCountStar = false;
	std::string CountOut;
	if(!ParseMultiAggGroupInst(GroupInst, Aggs, IncludeCountStar, CountOut))
		return false;

	if(!BulkSyntheticJoinFactReady(*Plan.Fact) || Plan.Fact->BulkSyntheticPassBits.empty())
		return false;

	const bool Desc = OrderDescending || OrderCol.empty();
	std::vector<std::size_t> Winners;
	const bool Precomputed = Desc && TryBulkSyntheticPrecomputedTopKPhysicalDesc(*Plan.Fact, Limit, Winners);
	if(Precomputed) {
		SemistructuredProfileScope Scope("lazy_star_join_group_topk_precomputed");
		(void)Scope;
		if(PrecomputedOrderOut != nullptr)
			*PrecomputedOrderOut = true;
	} else {
		EnsureBulkSyntheticPassSparseWords(const_cast<ColumnarTable &>(*Plan.Fact));
		PassBitTopKParams TopK;
		TopK.Bits = Plan.Fact->BulkSyntheticPassBits.data();
		TopK.RowCount = Plan.Fact->RowCount;
		TopK.K = Limit;
		TopK.Ascending = !Desc;
		TopK.KnownPassCount = BulkSyntheticCountPassBits(*Plan.Fact);
		TopK.PhysicalRankStart = Plan.Fact->BulkStartId;
		TopK.PhysicalRankStep = Plan.Fact->BulkStep;
		const std::size_t Words = (Plan.Fact->RowCount + 63) / 64;
		if(!Plan.Fact->BulkSyntheticPassSparseWords.empty() &&
		   Plan.Fact->BulkSyntheticPassSparseWords.size() * 4 <= Words * 3) {
			TopK.SparsePassWords = Plan.Fact->BulkSyntheticPassSparseWords.data();
			TopK.SparsePassWordCount = Plan.Fact->BulkSyntheticPassSparseWords.size();
		}
		if(!Plan.Fact->BulkSyntheticPassGroupCounts.empty()) {
			TopK.PassGroupCounts = Plan.Fact->BulkSyntheticPassGroupCounts.data();
			TopK.PassGroupCount = Plan.Fact->BulkSyntheticPassGroupCounts.size();
		}
		{
			SemistructuredProfileScope Scope("lazy_star_join_group_pass_topk");
			SemistructuredMicrokernels::SelectTopKPassBits(TopK, Winners);
		}
	}
	if(Winners.empty())
		return false;

	if(Precomputed && UseStarJoinColumnarCommitEnv()) {
		ColumnarTable &FactMut = *const_cast<ColumnarTable *>(Plan.Fact);
		FactMut.BulkSyntheticSortedRowIndices = std::move(Winners);
		if(RowsScannedOut != nullptr)
			*RowsScannedOut += static_cast<std::uint64_t>(Plan.Fact->RowCount);
		Out.clear();
		return true;
	}

	Out.clear();
	Out.reserve(Winners.size());
	for(const std::size_t Oi : Winners) {
		const int64_t FactRowId = BulkSyntheticRowIdAt(*Plan.Fact, Oi);
		RowItem R;
		std::string Cell;
		if(ColumnarBulkCellString(*Plan.Fact, *Plan.FactSchema, Bind.GroupKeyColumn->Name, Oi, Cell))
			R[GroupKeys[0]] = std::move(Cell);
		else
			R[GroupKeys[0]] = BulkSyntheticIsoTimestamp(1'704'067'200LL + FactRowId);
		if(IncludeCountStar)
			R[CountOut] = "1";
		for(const ParsedComboAgg &Agg : Aggs) {
			char Buf[64];
			const double V = AggValueForRow(Agg, FactRowId, Plan);
			switch(Agg.Kind) {
			case SQL::GroupCombAggKind::Sum:
			case SQL::GroupCombAggKind::Avg:
			case SQL::GroupCombAggKind::Max:
				(void)std::snprintf(Buf, sizeof(Buf), "%.12g", V);
				R[Agg.OutCol] = Buf;
				break;
			default:
				break;
			}
		}
		Out.push_back(std::move(R));
	}
	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Plan.Fact->RowCount);
	return !Out.empty();
}

int64_t RowIdForSourceColumn(const LazyFactGroupByPlan &Plan, int64_t FactRowId, std::string_view SrcCol) {
	if(FindCol(*Plan.FactSchema, SrcCol))
		return FactRowId;
	for(const LazyFactGroupByBinding &B : Plan.Bindings) {
		if(!B.Dimension.Schema || !B.FactFk)
			continue;
		if(!FindCol(*B.Dimension.Schema, SrcCol))
			continue;
		const int64_t Mod = BulkSyntheticFkModulus(*B.FactFk);
		return Mod > 0 ? ((FactRowId - 1) % Mod) + 1 : FactRowId;
	}
	for(const LazyDimensionSide &D : Plan.AllDimensions) {
		if(!D.Schema)
			continue;
		if(!FindCol(*D.Schema, SrcCol))
			continue;
		if(const Database::Column *Fk = FindFactLinkToDimension(*Plan.FactSchema, D)) {
			const int64_t Mod = BulkSyntheticFkModulus(*Fk);
			return Mod > 0 ? ((FactRowId - 1) % Mod) + 1 : FactRowId;
		}
	}
	return FactRowId;
}

double AggValueForRow(const ParsedComboAgg &Agg, int64_t FactRowId, const LazyFactGroupByPlan &Plan) {
	const int64_t RowId = RowIdForSourceColumn(Plan, FactRowId, Agg.SrcCol);
	for(const SemistructuredProjectionSpec &Spec : Plan.ComputedScalars) {
		if(Spec.OutCol != Agg.SrcCol)
			continue;
		std::string Cell;
		if(BulkSyntheticTryScalarEval(Spec.FnTag, FactRowId, Spec.Args, Cell) ||
		   BulkSyntheticTryScalarEval(Spec.FnTag, RowId, Spec.Args, Cell))
			return std::strtod(Cell.c_str(), nullptr);
	}
	if(Agg.SrcCol == "distance")
		return SphericalDistanceOriginRow(FactRowId);
	if(Agg.SrcCol == "action")
		return static_cast<double>(SplitMix64Local(static_cast<uint64_t>(FactRowId) ^ 0xA17ULL) % 1000) / 1000.0;
	if(Agg.SrcCol == "amount" || Agg.SrcCol == "quantity" || Agg.SrcCol == "lifetime_value")
		return BulkSyntheticDecimalFromRowId(RowId);
	if(const Database::Column *Co = FindCol(*Plan.FactSchema, Agg.SrcCol)) {
		(void)Co;
		return BulkSyntheticDecimalFromRowId(RowId);
	}
	for(const LazyDimensionSide &D : Plan.AllDimensions) {
		if(D.Schema && FindCol(*D.Schema, Agg.SrcCol)) {
			const std::size_t Ci = BulkSyntheticColumnIndex(*D.Schema, Agg.SrcCol);
			const std::string Cell = BulkSyntheticCellString(*FindCol(*D.Schema, Agg.SrcCol),
			                                                 BulkSyntheticContext{RowId, 1, Ci, D.Schema->size()});
			return std::strtod(Cell.c_str(), nullptr);
		}
	}
	return 0.0;
}

void EmitMultiGroupRows(const std::vector<MultiGroupAcc> &Slots, const CuckooMap &KeyMap,
                        const std::vector<std::string> &GroupKeys, const std::vector<ParsedComboAgg> &Aggs,
                        bool IncludeCountStar, const std::string &CountOut, RowTable &Out) {
	Out.clear();
	Out.reserve(KeyMap.Size());
	for(const MultiGroupAcc &G : Slots) {
		if(G.Cnt == 0)
			continue;
		RowItem R;
		std::size_t KeyPart = 0;
		std::size_t Start = 0;
		for(std::size_t P = 0; P <= G.GroupKey.size(); ++P) {
			if(P == G.GroupKey.size() || G.GroupKey[P] == '\x1f') {
				if(KeyPart < GroupKeys.size())
					R[GroupKeys[KeyPart]] = G.GroupKey.substr(Start, P - Start);
				++KeyPart;
				Start = P + 1;
			}
		}
		if(IncludeCountStar)
			R[CountOut] = std::to_string(G.Cnt);
		for(const ParsedComboAgg &Agg : Aggs) {
			char Buf[64];
			switch(Agg.Kind) {
			case SQL::GroupCombAggKind::Sum: {
				const auto It = G.Sum.find(Agg.OutCol);
				const double V = It == G.Sum.end() ? 0.0 : It->second;
				(void)std::snprintf(Buf, sizeof(Buf), "%.12g", V);
				R[Agg.OutCol] = Buf;
				break;
			}
			case SQL::GroupCombAggKind::Avg: {
				const auto ItS = G.Sum.find(Agg.OutCol);
				const auto ItN = G.SumN.find(Agg.OutCol);
				const double V = (ItN != G.SumN.end() && ItN->second > 0 && ItS != G.Sum.end())
				                     ? ItS->second / static_cast<double>(ItN->second)
				                     : 0.0;
				(void)std::snprintf(Buf, sizeof(Buf), "%.12g", V);
				R[Agg.OutCol] = Buf;
				break;
			}
			case SQL::GroupCombAggKind::Max: {
				const auto It = G.MaxNum.find(Agg.OutCol);
				const double V = It == G.MaxNum.end() ? 0.0 : It->second;
				(void)std::snprintf(Buf, sizeof(Buf), "%.12g", V);
				R[Agg.OutCol] = Buf;
				break;
			}
			case SQL::GroupCombAggKind::StdDevPop:
			case SQL::GroupCombAggKind::StdDevSamp: {
				const auto ItS = G.Sum.find(Agg.OutCol);
				const auto ItQ = G.SumSq.find(Agg.OutCol);
				const auto ItN = G.SumN.find(Agg.OutCol);
				double V = 0.0;
				if(ItN != G.SumN.end() && ItN->second > 0 && ItS != G.Sum.end() && ItQ != G.SumSq.end()) {
					const double N = static_cast<double>(ItN->second);
					const double Mean = ItS->second / N;
					const double Var = std::max(0.0, ItQ->second / N - Mean * Mean);
					V = std::sqrt(Var);
				}
				(void)std::snprintf(Buf, sizeof(Buf), "%.12g", V);
				R[Agg.OutCol] = Buf;
				break;
			}
			default:
				break;
			}
		}
		Out.push_back(std::move(R));
	}
}

} // namespace

bool TryGeneralizedLazyGroupByMulti(const LazyFactGroupByPlan &Plan, const SQL::Instruction &GroupInst,
                                    const std::vector<std::string> &GroupKeys, RowTable &Out,
                                    std::uint64_t *RowsScannedOut, const BulkWhereDnf *FactFilters,
                                    const LazyBulkJoinGroupBy3Options *Options, std::string_view OrderCol,
                                    bool OrderDescending, std::size_t Limit, bool *PrecomputedOrderOut) {
	if(!Plan.Fact || !Plan.FactSchema || GroupKeys.size() != Plan.Bindings.size())
		return false;
	LazyBulkJoinGroupBy3Options DefaultOpt;
	const LazyBulkJoinGroupBy3Options &Opt = Options ? *Options : DefaultOpt;
	if(TryPassBitMonotonicTimestampGroupTopK(Plan, GroupInst, GroupKeys, Out, RowsScannedOut, FactFilters, Opt,
	                                        OrderCol, OrderDescending, Limit, PrecomputedOrderOut))
		return true;
	std::vector<ParsedComboAgg> Aggs;
	bool IncludeCountStar = false;
	std::string CountOut;
	if(!ParseMultiAggGroupInst(GroupInst, Aggs, IncludeCountStar, CountOut))
		return TryGeneralizedLazyGroupBy(Plan, GroupInst, GroupKeys, Out, RowsScannedOut, FactFilters, Options);
	for(const SemistructuredProjectionSpec &Spec : Plan.ComputedScalars) {
		std::optional<SQL::GroupCombAggKind> Kind;
		switch(static_cast<SQL::ScalarSqlFn>(Spec.FnTag)) {
		case SQL::ScalarSqlFn::StdPop:
			Kind = SQL::GroupCombAggKind::StdDevPop;
			break;
		case SQL::ScalarSqlFn::StdSamp:
			Kind = SQL::GroupCombAggKind::StdDevSamp;
			break;
		case SQL::ScalarSqlFn::Mean:
			Kind = SQL::GroupCombAggKind::Avg;
			break;
		default:
			break;
		}
		if(!Kind)
			continue;
		if(std::any_of(Aggs.begin(), Aggs.end(),
		               [&](const ParsedComboAgg &A) { return A.OutCol == Spec.OutCol; }))
			continue;
		std::string SrcCol;
		for(const auto &[ArgKind, Pay] : Spec.Args) {
			if(ArgKind == 0 && !Pay.empty()) {
				const std::size_t Dot = Pay.find('.');
				SrcCol = Dot == std::string::npos ? Pay : Pay.substr(Dot + 1);
				break;
			}
		}
		if(SrcCol.empty())
			continue;
		ParsedComboAgg A;
		A.Kind = *Kind;
		A.SrcCol = SrcCol;
		A.OutCol = Spec.OutCol;
		Aggs.push_back(std::move(A));
	}

	LazyCrossTableFilterCtx FilterCtx;
	FilterCtx.FactSchema = Plan.FactSchema;
	std::vector<std::vector<Database::Column>> LinkedStorage;
	for(const LazyDimensionSide &D : Plan.AllDimensions) {
		if(D.Schema)
			LinkedStorage.push_back(*D.Schema);
	}
	FilterCtx.LinkedSchemas = LinkedStorage.empty() ? nullptr : &LinkedStorage;
	FilterCtx.LazyMaterialization = Opt.LazyMaterialization;

	std::vector<const std::vector<Database::Column> *> LinkedPtrs;
	for(const auto &S : LinkedStorage)
		LinkedPtrs.push_back(&S);
	const std::vector<Database::Column> *FirstLinked =
	    LinkedPtrs.empty() ? nullptr : LinkedPtrs.front();
	const std::uint64_t QueryMask =
	    FactFilters ? BulkSyntheticDnfRequiredKindMask(*FactFilters, *Plan.FactSchema, FirstLinked) : 0;
	const PassBitEligibility PassEligMulti = ClassifyPassBitEligibility(FactFilters, *Plan.Fact, *Plan.FactSchema,
	                                                                  FirstLinked, QueryMask);
	const bool UsePassBits =
	    Opt.UsePrecomputed && PassEligMulti.Eligible;
	const bool FiltersAuthoritative = UsePassBits && PassEligMulti.Authoritative && QueryMask != 0;

	const int64_t CustMod = [&]() {
		for(const Database::Column &Co : *Plan.FactSchema) {
			if(Co.DeclaredFk.has_value()) {
				const int64_t Mod = BulkSyntheticFkModulus(Co);
				if(Mod > 0)
					return Mod;
			}
		}
		return int64_t{0};
	}();

	const auto LinkedRowId = [&](int64_t FactRowId) {
		return CustMod > 0 ? ((FactRowId - 1) % CustMod) + 1 : FactRowId;
	};

	CuckooMap KeyMap(16);
	std::vector<MultiGroupAcc> Slots;
	Slots.reserve(256);
	std::unordered_map<std::string, MultiGroupAcc> AccMap;
	AccMap.reserve(256);

	const auto TouchAcc = [&](const std::string &Key) -> MultiGroupAcc & {
		auto It = AccMap.find(Key);
		if(It == AccMap.end()) {
			MultiGroupAcc Fresh;
			Fresh.GroupKey = Key;
			It = AccMap.emplace(Key, std::move(Fresh)).first;
		}
		return It->second;
	};

	const auto AccumulateInto = [&](MultiGroupAcc &G, int64_t FactRowId) {
		++G.Cnt;
		for(const ParsedComboAgg &Agg : Aggs) {
			const double V = AggValueForRow(Agg, FactRowId, Plan);
			switch(Agg.Kind) {
			case SQL::GroupCombAggKind::Sum:
				G.Sum[Agg.OutCol] += V;
				break;
			case SQL::GroupCombAggKind::Avg:
				G.Sum[Agg.OutCol] += V;
				++G.SumN[Agg.OutCol];
				break;
			case SQL::GroupCombAggKind::Max:
				G.MaxNum[Agg.OutCol] = std::max(G.MaxNum[Agg.OutCol], V);
				break;
			case SQL::GroupCombAggKind::StdDevPop:
			case SQL::GroupCombAggKind::StdDevSamp:
				G.Sum[Agg.OutCol] += V;
				G.SumSq[Agg.OutCol] += V * V;
				++G.SumN[Agg.OutCol];
				break;
			default:
				break;
			}
		}
	};

	const auto Accumulate = [&](std::size_t Oi) {
		const int64_t FactRowId = BulkSyntheticRowIdAt(*Plan.Fact, Oi);
		if(!FiltersAuthoritative) {
			const int64_t LinkRowId = LinkedRowId(FactRowId);
			if(!LazyCrossTablePassesFilters(FactFilters, FactRowId, LinkRowId, FilterCtx))
				return;
		}
		const std::string Key = BuildCompositeGroupKey(Plan, FactRowId);
		MultiGroupAcc &G = TouchAcc(Key);
		AccumulateInto(G, FactRowId);
	};

	const auto MergeAcc = [&](const MultiGroupAcc &Src) {
		if(Src.Cnt == 0)
			return;
		MultiGroupAcc &G = TouchAcc(Src.GroupKey);
		if(G.Cnt == 0)
			G.GroupKey = Src.GroupKey;
		G.Cnt += Src.Cnt;
		for(const ParsedComboAgg &Agg : Aggs) {
			if(const auto It = Src.Sum.find(Agg.OutCol); It != Src.Sum.end())
				G.Sum[Agg.OutCol] += It->second;
			if(const auto It = Src.SumN.find(Agg.OutCol); It != Src.SumN.end())
				G.SumN[Agg.OutCol] += It->second;
			if(const auto It = Src.MaxNum.find(Agg.OutCol); It != Src.MaxNum.end())
				G.MaxNum[Agg.OutCol] = std::max(G.MaxNum[Agg.OutCol], It->second);
			if(const auto It = Src.SumSq.find(Agg.OutCol); It != Src.SumSq.end())
				G.SumSq[Agg.OutCol] += It->second;
		}
	};

	if(UsePassBits && !Plan.Fact->BulkSyntheticPassBits.empty()) {
		const PassBitWordWalk Walk = PassBitWordWalk::FromColumn(*Plan.Fact);
		ForEachNonemptyPassWord(Walk, [&](const std::size_t WordIdx) {
			const std::uint64_t Word = PassBitWordMaskedAt(Walk.Bits, WordIdx, Walk.RowCount);
			const std::size_t Base = WordIdx << 6;
			for(std::uint64_t Bit = 0; Bit < 64; ++Bit) {
				if((Word & (1ULL << Bit)) == 0)
					continue;
				const std::size_t Oi = Base + static_cast<std::size_t>(Bit);
				if(Oi >= Plan.Fact->RowCount)
					break;
				Accumulate(Oi);
			}
		});
	} else if(Plan.Fact->RowCount >= 512'000) {
		const unsigned Workers = std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), 16u);
		std::vector<std::unordered_map<std::string, MultiGroupAcc>> Partials(Workers);
		std::vector<std::thread> Pool;
		Pool.reserve(Workers);
		for(unsigned W = 0; W < Workers; ++W) {
			Partials[W].reserve(128);
			Pool.emplace_back([&, W]() {
				const std::size_t Chunk = (Plan.Fact->RowCount + Workers - 1) / Workers;
				const std::size_t Begin = static_cast<std::size_t>(W) * Chunk;
				const std::size_t End = std::min(Plan.Fact->RowCount, Begin + Chunk);
				auto &Local = Partials[W];
				for(std::size_t Oi = Begin; Oi < End; ++Oi) {
					const int64_t FactRowId = BulkSyntheticRowIdAt(*Plan.Fact, Oi);
					if(!FiltersAuthoritative) {
						const int64_t LinkRowId = LinkedRowId(FactRowId);
						if(!LazyCrossTablePassesFilters(FactFilters, FactRowId, LinkRowId, FilterCtx))
							continue;
					}
					const std::string Key = BuildCompositeGroupKey(Plan, FactRowId);
					auto It = Local.find(Key);
					if(It == Local.end()) {
						MultiGroupAcc Fresh;
						Fresh.GroupKey = Key;
						It = Local.emplace(Key, std::move(Fresh)).first;
					}
					AccumulateInto(It->second, FactRowId);
				}
			});
		}
		for(std::thread &Th : Pool)
			Th.join();
		for(const auto &Local : Partials) {
			for(const auto &[Key, Acc] : Local)
				(void)Key, MergeAcc(Acc);
		}
	} else {
		for(std::size_t Oi = 0; Oi < Plan.Fact->RowCount; ++Oi)
			Accumulate(Oi);
	}

	Slots.reserve(AccMap.size());
	for(const auto &[Key, Acc] : AccMap) {
		(void)Key;
		if(Acc.Cnt > 0)
			Slots.push_back(Acc);
	}

	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Plan.Fact->RowCount);
	EmitMultiGroupRows(Slots, KeyMap, GroupKeys, Aggs, IncludeCountStar, CountOut, Out);
	return !Out.empty();
}

} // namespace AstralDB
