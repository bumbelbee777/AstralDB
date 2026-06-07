#include <Database/Storage/BulkSyntheticSemistructured.hxx>

#include <Database/Storage/BulkSyntheticPathEval.hxx>
#include <Database/Database.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>
#include <Database/Storage/SemistructuredResultStrips.hxx>

#include <Database/Storage/BulkSyntheticPrecomputeColumns.hxx>
#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/PassBitWalk.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/SemistructuredMicrokernels.hxx>
#include <Database/Text/PatternMatch.hxx>
#include <Database/MathSci/MathSciInference.hxx>
#include <Database/MathSci/MathSciFokkerPlanck.hxx>
#include <Database/MathSci/MathSci.hxx>

#include <DS/SimdJsonExtract.hxx>
#include <DS/SimdXmlExtract.hxx>

#include <algorithm>
#include <cmath>
#include <sstream>

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

bool BioKeywordRow(int64_t RowId) noexcept {
	const std::string Text = BulkSyntheticBioText(RowId);
	return Text.rfind("Biography mentioning", 0) == 0;
}

std::string_view NormalizeJsonPath(std::string_view Path) noexcept {
	if(Path.size() >= 2 && Path[0] == '$' && Path[1] == '.')
		return Path.substr(2);
	if(!Path.empty() && Path[0] == '$')
		return Path.substr(1);
	return Path;
}

} // namespace

bool BulkSyntheticJsonExtractRow(const int64_t RowId, const std::string_view Path, std::string &Out) noexcept {
	const BulkSyntheticJsonPathPlan Plan = PlanBulkSyntheticJsonPath(Path);
	if(Plan.Leaf != BulkSyntheticJsonLeaf::Unknown)
		return BulkSyntheticJsonExtractPlanned(RowId, Plan, Path, Out);
	const std::string Json = BulkSyntheticJsonCell(RowId, 0);
	return SimdJsonExtract::Extract(Json, NormalizeJsonPath(Path), Out);
}

bool BulkSyntheticXmlExtractRow(const int64_t RowId, const std::string_view Path, std::string &Out) noexcept {
	const BulkSyntheticXmlPathPlan Plan = PlanBulkSyntheticXmlPath(Path);
	if(Plan.Leaf != BulkSyntheticXmlLeaf::Unknown)
		return BulkSyntheticXmlExtractPlanned(RowId, Plan, Path, Out);
	const std::string Xml = BulkSyntheticXmlCell(RowId);
	return SimdXmlExtract::Extract(Xml, Path, Out);
}

bool BulkSyntheticXmlValidRow(const int64_t RowId) noexcept {
	(void)RowId;
	return true;
}

std::size_t BulkSyntheticBioLengthRow(const int64_t RowId) noexcept {
	return BulkSyntheticBioText(RowId).size();
}

double BulkSyntheticBioRankRow(const int64_t RowId) noexcept {
	const uint64_t R = RowSeed(RowId, 0xB10ULL);
	if(!BioKeywordRow(RowId))
		return static_cast<double>(R % 250) / 1000.0;
	return 0.5 + static_cast<double>(R % 500) / 1000.0;
}

float BulkSyntheticBioRankF32Row(const int64_t RowId) noexcept {
	const uint64_t R = RowSeed(RowId, 0xB10ULL);
	if(!BioKeywordRow(RowId))
		return static_cast<float>(R % 250) * 0.001f;
	return 0.5f + static_cast<float>(R % 500) * 0.001f;
}

float BulkSyntheticReviewMatchF32Row(const int64_t RowId) noexcept {
	return (RowSeed(RowId, 0xBEA7E7ULL) & 3) == 0 ? 1.0f : 0.0f;
}

void EnsureBulkSyntheticLazyRankF32(ColumnarTable &Col) noexcept {
	SemistructuredMicrokernels::FillBioRankF32Column(Col);
}

bool BulkSyntheticRegexpExtractRow(const int64_t RowId, const std::string_view Pattern, std::string &Out) noexcept {
	const std::string Text = BulkSyntheticBioText(RowId);
	const auto Got = SqlRegexpExtract(Text, std::string(Pattern), 1, false);
	if(Got) {
		Out = *Got;
		return true;
	}
	Out.clear();
	return true;
}

void CollectBulkSyntheticPassingIndices(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        const Database *Db, const std::string &ContextTable,
                                        std::vector<std::size_t> &OutIndices) noexcept {
	OutIndices.clear();
	if(Col.RowCount == 0)
		return;
	if(!Col.BulkSyntheticPassBits.empty()) {
		ScanPassBitsIndices(Col.BulkSyntheticPassBits.data(), Col.RowCount, OutIndices);
		return;
	}
	for(std::size_t I = 0; I < Col.RowCount; ++I) {
		if(BulkSyntheticRowPassesWhereStackFast(Col, Schema, I, Db, ContextTable))
			OutIndices.push_back(I);
	}
}

namespace {

bool ScalarFuncArgsAllLiteral(const std::vector<std::pair<int64_t, std::string>> &Args) noexcept {
	if(Args.empty())
		return false;
	for(const auto &[Kind, Pay] : Args) {
		(void)Pay;
		if(Kind != 0 && Kind != 5)
			return false;
	}
	return true;
}

std::optional<std::string> EvalConstScalarFunc(const int FnTag,
                                               const std::vector<std::pair<int64_t, std::string>> &Args) {
	using ScalarSqlFn = SQL::ScalarSqlFn;
	const auto Fn = static_cast<ScalarSqlFn>(FnTag);
	if(Fn == ScalarSqlFn::MctsSearch && Args.size() >= 4)
		return MathSciInference::MctsSearchCellFromReal(Args[0].second, Args[1].second, Args[2].second,
		                                                Args[3].second);
	if(Fn == ScalarSqlFn::NfpMacroMarch && Args.size() >= 10)
		return MathSciFokkerPlanck::NfpMacroMarchCellFromReal(Args[0].second, Args[1].second, Args[2].second,
		                                                      Args[3].second, Args[4].second, Args[5].second,
		                                                      Args[6].second, Args[7].second, Args[8].second,
		                                                      Args[9].second);
	if(Fn == ScalarSqlFn::NfpMacroMoments && Args.size() >= 2)
		return MathSciFokkerPlanck::NfpMacroMomentsCellFromReal(Args[0].second, Args[1].second);
	std::vector<std::string> Cells;
	Cells.reserve(Args.size());
	for(const auto &[Kind, Pay] : Args) {
		(void)Kind;
		Cells.push_back(Pay);
	}
	return MathSci::EvalScalar(Fn, Cells, nullptr);
}

} // namespace

bool ApplyLazyScalarProjection(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const Database *Db,
                               const std::string &ContextTable, const int FnTag,
                               const std::vector<std::pair<int64_t, std::string>> &Args,
                               const std::string &OutColName) noexcept {
	(void)Schema;
	if(OutColName.empty() || Col.RowCount == 0)
		return false;
	std::vector<std::string> &OutCol = Col.Columns[OutColName];
	OutCol.resize(Col.RowCount);
	if(ScalarFuncArgsAllLiteral(Args)) {
		if(const std::optional<std::string> Val = EvalConstScalarFunc(FnTag, Args)) {
			for(std::string &Cell : OutCol)
				Cell = *Val;
			return true;
		}
	}
	if(Db != nullptr && !ContextTable.empty()) {
		HybridTableSlot *Slot = const_cast<Database *>(Db)->FindTableSlotAssumeDbMutexHeld(ContextTable);
		if(Slot != nullptr && Slot->BroadcastCrossJoinRhsRow.has_value()) {
			const HybridTableSlot::Item &Rhs = *Slot->BroadcastCrossJoinRhsRow;
			std::vector<std::pair<int64_t, std::string>> ResolvedArgs;
			ResolvedArgs.reserve(Args.size());
			bool AllConst = true;
			for(const auto &[Kind, Pay] : Args) {
				if(Kind == 0 || Kind == 5) {
					ResolvedArgs.emplace_back(Kind, Pay);
					continue;
				}
				if(Kind != 1) {
					AllConst = false;
					break;
				}
				std::string_view ColName = Pay;
				const std::size_t Dot = Pay.find('.');
				if(Dot != std::string::npos)
					ColName = Pay.substr(Dot + 1);
				if(const auto It = Rhs.find(std::string(ColName)); It != Rhs.end()) {
					ResolvedArgs.emplace_back(0, It->second);
					continue;
				}
				const auto ColIt = Col.Columns.find(std::string(ColName));
				if(ColIt != Col.Columns.end() && !ColIt->second.empty()) {
					ResolvedArgs.emplace_back(0, ColIt->second.front());
					continue;
				}
				AllConst = false;
				break;
			}
			if(AllConst && ScalarFuncArgsAllLiteral(ResolvedArgs)) {
				if(const std::optional<std::string> Val = EvalConstScalarFunc(FnTag, ResolvedArgs)) {
					for(std::string &Cell : OutCol)
						Cell = *Val;
					return true;
				}
			}
		}
	}
	for(std::size_t I = 0; I < Col.RowCount; ++I) {
		const int64_t RowId = BulkSyntheticRowIdAt(Col, I);
		std::string Cell;
		if(BulkSyntheticTryScalarEval(FnTag, RowId, Args, Cell))
			OutCol[I] = std::move(Cell);
		else
			OutCol[I].clear();
	}
	return true;
}

bool TryLazyBulkOrderByColumnar(ColumnarTable &Col, const std::string &SortCol, const bool Ascending,
                                const std::size_t TopKeep) noexcept {
	(void)SortCol;
	(void)Ascending;
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0 || TopKeep == 0)
		return false;
	std::vector<std::size_t> Winners;
	if(!TryBulkSyntheticPrecomputedTopKDesc(Col, TopKeep, Winners))
		return false;
	Col.BulkSyntheticSortedRowIndices = std::move(Winners);
	return true;
}

bool MaterializeSemistructuredWinners(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                      const std::vector<SemistructuredProjectionSpec> &Projections,
                                      const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept {
	ColumnarTable &MutCol = const_cast<ColumnarTable &>(Col);
	if(MaterializeWinnersLutRowStore(MutCol, Projections, Schema, WinnerRowIndices, Out))
		return true;
	return SemistructuredMicrokernels::MaterializeWinnersBatched(Col, Schema, Projections, WinnerRowIndices, Out);
}

namespace {

bool OrderUsesTextRank(const SemistructuredProjectionSpec *OrderSpec) noexcept {
	return OrderSpec != nullptr && static_cast<SQL::ScalarSqlFn>(OrderSpec->FnTag) == SQL::ScalarSqlFn::TextRank;
}

const SemistructuredProjectionSpec *FindOrderSpec(const std::vector<SemistructuredProjectionSpec> &Projections,
                                                  const std::string &OrderCol) noexcept {
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol == OrderCol)
			return &P;
	}
	return nullptr;
}

bool SelectWinnersFromPassBits(ColumnarTable &Col, const SemistructuredProjectionSpec *OrderSpec,
                               const std::string & /*OrderCol*/, const bool OrderAscending, const std::size_t Limit,
                               std::vector<std::size_t> &Winners) noexcept {
	if(Col.BulkSyntheticPassBits.empty())
		return false;
	EnsureBulkSyntheticPassSparseWords(Col);
	PassBitTopKParams Params;
	Params.Bits = Col.BulkSyntheticPassBits.data();
	Params.RowCount = Col.RowCount;
	Params.K = Limit;
	Params.Ascending = OrderAscending;
	const std::size_t Words = (Col.RowCount + 63) / 64;
	if(!Col.BulkSyntheticPassSparseWords.empty() && Col.BulkSyntheticPassSparseWords.size() * 4 <= Words * 3) {
		Params.SparsePassWords = Col.BulkSyntheticPassSparseWords.data();
		Params.SparsePassWordCount = Col.BulkSyntheticPassSparseWords.size();
	}
	if(!Col.BulkSyntheticPassGroupCounts.empty()) {
		Params.PassGroupCounts = Col.BulkSyntheticPassGroupCounts.data();
		Params.PassGroupCount = Col.BulkSyntheticPassGroupCounts.size();
	}
	if(!Col.BulkSyntheticPassGroupMaxRankF32.empty())
		Params.PassGroupMaxRank = Col.BulkSyntheticPassGroupMaxRankF32.data();
	Params.KnownPassCount = BulkSyntheticCountPassBits(Col);
	const bool RankOrder = OrderUsesTextRank(OrderSpec);
	const bool ComputedRankTopK = RankOrder && Col.BulkSyntheticPhysicalOrder && Col.BulkStep != 0 &&
	                              Col.BulkSyntheticLazyRankF32.size() != Col.RowCount;
	if(RankOrder && !ComputedRankTopK && Col.BulkSyntheticLazyRankF32.size() != Col.RowCount &&
	   !(Col.BulkSyntheticPhysicalOrder && Col.BulkStep != 0))
		EnsureBulkSyntheticLazyRankF32(Col);
	if(ComputedRankTopK || (RankOrder && Col.BulkSyntheticPhysicalOrder && Col.BulkStep != 0 &&
	                        Col.BulkSyntheticLazyRankF32.size() != Col.RowCount)) {
		Params.PhysicalRankStart = Col.BulkStartId;
		Params.PhysicalRankStep = Col.BulkStep;
	} else if(RankOrder && Col.BulkSyntheticLazyRankF32.size() == Col.RowCount) {
		Params.Keys = Col.BulkSyntheticLazyRankF32.data();
	} else if(RankOrder) {
		Params.KeyFn = [&](const std::size_t RowIndex) -> float {
			return BulkSyntheticBioRankF32Row(BulkSyntheticRowIdAt(Col, RowIndex));
		};
	} else {
		Params.KeyFn = [&](const std::size_t RowIndex) -> float {
			return static_cast<float>(BulkSyntheticRowIdAt(Col, RowIndex));
		};
	}
	SemistructuredMicrokernels::SelectTopKPassBits(Params, Winners);
	return !Winners.empty();
}

} // namespace

bool ExecuteFusedSemistructuredScan(Database &Db, const std::string &Table, const BulkWhereDnf &FilterDnf,
                                    const std::vector<SemistructuredProjectionSpec> &Projections,
                                    const std::string &OrderCol, const bool OrderAscending, const std::size_t Limit,
                                    RowTable &Out, std::uint64_t *RowsScannedOut, bool *ColumnarCommittedOut) noexcept {
	Out.clear();
	if(Table.empty() || Limit == 0)
		return false;
	HybridTableSlot *Slot = Db.FindTableSlotAssumeDbMutexHeld(Table);
	if(!Slot || Slot->Columnar.RowCount == 0)
		return false;
	const auto Schema = Db.TableSchemaAssumeDbMutexHeld(Table);
	if(!Schema || Schema->empty())
		return false;
	ColumnarTable &Col = Slot->Columnar;
	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Col.RowCount);
	if(ColumnarCommittedOut != nullptr)
		*ColumnarCommittedOut = false;

	const SemistructuredProjectionSpec *OrderSpec = FindOrderSpec(Projections, OrderCol);
	(void)FilterDnf;

	{
		SemistructuredProfileScope FusedScope("semistructured_fused_scan");
		std::vector<std::size_t> Winners;
		if(TryBulkSyntheticPrecomputedTopKDesc(Col, Limit, Winners)) {
			SemistructuredProfileScope PreScope("semistructured_topk_precomputed");
			(void)PreScope;
		} else if(!Col.BulkSyntheticPassBits.empty()) {
			if(!SelectWinnersFromPassBits(Col, OrderSpec, OrderCol, OrderAscending, Limit, Winners))
				return true;
		} else {
			return false;
		}

		if(ColumnarCommittedOut != nullptr) {
			SemistructuredProfileScope CommitScope("semistructured_columnar_commit");
			if(TryCommitSemistructuredColumnarResult(Col, Projections, *Schema, static_cast<std::uint32_t>(Limit))) {
				*ColumnarCommittedOut = true;
				SemistructuredProfileScope DoneScope("semistructured_columnar_committed");
				(void)DoneScope;
				return true;
			}
		}
		return MaterializeSemistructuredWinners(Col, *Schema, Projections, Winners, Out);
	}
}

} // namespace AstralDB
