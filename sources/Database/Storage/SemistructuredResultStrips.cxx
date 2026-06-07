#include <Database/Storage/SemistructuredResultStrips.hxx>

#include <Database/Storage/SemistructuredProfile.hxx>
#include <Database/Profile/RegionTimer.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/HybridTable.hxx>
#include <Database/Storage/BulkSyntheticPrecomputeColumns.hxx>
#include <Database/Storage/SemistructuredLut.hxx>
#include <Database/Storage/SemistructuredResultPolicy.hxx>
#include <DS/FormatDoubleSimd.hxx>
#include <IO/Job.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <algorithm>
#include <cstring>
#include <future>

namespace AstralDB {

using ScalarSqlFn = SQL::ScalarSqlFn;

namespace {

std::uint64_t Mix64(std::uint64_t H, std::uint64_t V) noexcept {
	H ^= V + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
	return H;
}

std::uint64_t HashString(std::string_view S) noexcept {
	std::uint64_t H = 0xcbf29ce484222325ULL;
	for(unsigned char C : S)
		H = Mix64(H, C);
	return H;
}

const Database::Column *FindPrimaryKeyColumn(const std::vector<Database::Column> &Schema) noexcept {
	for(const Database::Column &C : Schema) {
		if(C.IsPrimaryKey)
			return &C;
	}
	return Schema.empty() ? nullptr : &Schema.front();
}

void GatherWinnerRowIds(const ColumnarTable &Col, const std::vector<std::size_t> &Winners,
                        std::vector<int64_t> &Out) noexcept {
	const std::size_t N = Winners.size();
	Out.resize(N);
	if(Col.BulkSyntheticPhysicalOrder && Col.BulkStep == 1) {
		const int64_t Start = Col.BulkStartId;
		for(std::size_t I = 0; I < N; ++I)
			Out[I] = Start + static_cast<int64_t>(Winners[I]);
		return;
	}
	for(std::size_t I = 0; I < N; ++I)
		Out[I] = BulkSyntheticRowIdAt(Col, Winners[I]);
}

bool EnvParallelStrips() noexcept {
	const char *V = std::getenv("ASTRALDB_SEMISTRUCTURED_PARALLEL");
	return V == nullptr || (V[0] != '0' && V[0] != 'n' && V[0] != 'N');
}

void EmplaceCellFromStripView(RowItem &Row, const std::string &ColName, std::string_view View) noexcept {
	if(View.size() <= 24) {
		Row.emplace(ColName, std::string(View.data(), View.size()));
		return;
	}
	std::string Cell;
	Cell.resize(View.size());
	std::memcpy(Cell.data(), View.data(), View.size());
	Row.emplace(ColName, std::move(Cell));
}

[[nodiscard]] bool ShouldParallelMaterialize(const std::size_t TableRowCount, const std::size_t WinnerCount) noexcept {
	return EnvParallelStrips() && WinnerCount >= SemistructuredResultPolicy::ParallelMaterializeMinWinners(TableRowCount) &&
	       JobSystem::Instance().IsRunning();
}

void MaterializeStripZipBand(const BulkSyntheticProjectionStripPack &Pack, std::size_t RowBegin, std::size_t RowEnd,
                             RowTable &Out) {
	const std::size_t ColCount = Pack.ColumnNames.size();
	for(std::size_t Ri = RowBegin; Ri < RowEnd; ++Ri) {
		RowItem &Row = Out[Ri];
		Row.reserve(ColCount);
		for(std::size_t Ci = 0; Ci < Pack.Columns.size(); ++Ci) {
			if(Ri >= Pack.Columns[Ci].Lengths.size())
				continue;
			EmplaceCellFromStripView(Row, Pack.ColumnNames[Ci], Pack.Columns[Ci].View(Ri));
		}
	}
}

bool ProjectionRowIdBatchable(const SemistructuredProjectionSpec &P, const bool HasDenseRankKeys) noexcept {
	const auto Fn = static_cast<ScalarSqlFn>(P.FnTag);
	if(Fn == ScalarSqlFn::TextRank)
		return HasDenseRankKeys;
	if(Fn == ScalarSqlFn::CharLength)
		return true;
	if(Fn == ScalarSqlFn::JsonExtract || Fn == ScalarSqlFn::XmlExtract || Fn == ScalarSqlFn::RegexpExtract)
		return P.Args.size() >= 2;
	return false;
}

enum class LutMatColKind : std::uint8_t { Pk, JsonExtract, XmlExtract, RegexpExtract, CharLength, TextRank };

struct LutMatColumn {
	const std::string *Name = nullptr;
	LutMatColKind Kind = LutMatColKind::Pk;
	std::string_view Aux;
};

bool BuildLutMatColumns(const std::vector<SemistructuredProjectionSpec> &Projections, const Database::Column *Pk,
                        std::vector<LutMatColumn> &Cols) noexcept {
	Cols.clear();
	if(Pk != nullptr) {
		LutMatColumn C;
		C.Name = &Pk->Name;
		C.Kind = LutMatColKind::Pk;
		Cols.push_back(C);
	}
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		if(!ProjectionRowIdBatchable(P, true))
			return false;
		LutMatColumn C;
		C.Name = &P.OutCol;
		const auto Fn = static_cast<ScalarSqlFn>(P.FnTag);
		if(Fn == ScalarSqlFn::JsonExtract) {
			if(P.Args.size() < 2)
				return false;
			C.Kind = LutMatColKind::JsonExtract;
			C.Aux = P.Args[1].second;
		} else if(Fn == ScalarSqlFn::XmlExtract) {
			if(P.Args.size() < 2)
				return false;
			C.Kind = LutMatColKind::XmlExtract;
			C.Aux = P.Args[1].second;
		} else if(Fn == ScalarSqlFn::RegexpExtract) {
			if(P.Args.size() < 2)
				return false;
			C.Kind = LutMatColKind::RegexpExtract;
			C.Aux = P.Args[1].second;
		} else if(Fn == ScalarSqlFn::CharLength) {
			C.Kind = LutMatColKind::CharLength;
			if(!P.Args.empty())
				C.Aux = P.Args[0].second;
		} else if(Fn == ScalarSqlFn::TextRank) {
			C.Kind = LutMatColKind::TextRank;
			if(P.Args.size() >= 2)
				C.Aux = P.Args[1].second;
		} else {
			return false;
		}
		Cols.push_back(C);
	}
	return !Cols.empty();
}

void MaterializeLutRowBand(const std::vector<LutMatColumn> &Cols, const int64_t *RowIds,
                           const std::vector<std::size_t> &WinnerRowIndices, const float *RankKeys,
                           const std::size_t RankKeyCount, std::size_t RowBegin, std::size_t RowEnd, RowTable &Out) {
	for(std::size_t I = RowBegin; I < RowEnd; ++I) {
		RowItem &Row = Out[I];
		Row.reserve(Cols.size());
		const int64_t RowId = RowIds[I];
		for(const LutMatColumn &C : Cols) {
			switch(C.Kind) {
			case LutMatColKind::Pk:
				SemistructuredLut::EmplacePkCell(Row, *C.Name, RowId);
				break;
			case LutMatColKind::JsonExtract:
				SemistructuredLut::EmplaceJsonExtractCell(Row, *C.Name, RowId, C.Aux);
				break;
			case LutMatColKind::XmlExtract:
				SemistructuredLut::EmplaceXmlExtractCell(Row, *C.Name, RowId, C.Aux);
				break;
			case LutMatColKind::RegexpExtract:
				SemistructuredLut::EmplaceRegexpExtractCell(Row, *C.Name, RowId, C.Aux);
				break;
			case LutMatColKind::CharLength:
				SemistructuredLut::EmplaceCharLengthCell(Row, *C.Name, RowId);
				break;
			case LutMatColKind::TextRank: {
				const std::size_t Ri = WinnerRowIndices[I];
				const float Key = Ri < RankKeyCount ? RankKeys[Ri] : 0.f;
				SemistructuredLut::EmplaceRankCell(Row, *C.Name, Key);
				break;
			}
			}
		}
	}
}

bool FillStripForProjection(const SemistructuredProjectionSpec &P, const int64_t *RowIds, const std::size_t N,
                            const float *RankKeys, const std::size_t RankKeyCount,
                            const std::uint32_t *WinnerRowIndices,
                            FormatDoubleSimd::FormattedDoubleColumn &Out) noexcept {
	const auto Fn = static_cast<ScalarSqlFn>(P.FnTag);
	switch(Fn) {
	case ScalarSqlFn::JsonExtract:
		return P.Args.size() >= 2 &&
		       SemistructuredLut::FillJsonExtractStrip(RowIds, N, P.Args[1].second, Out);
	case ScalarSqlFn::XmlExtract:
		return P.Args.size() >= 2 && SemistructuredLut::FillXmlExtractStrip(RowIds, N, P.Args[1].second, Out);
	case ScalarSqlFn::RegexpExtract:
		return P.Args.size() >= 2 &&
		       SemistructuredLut::FillRegexpExtractStrip(RowIds, N, P.Args[1].second, Out);
	case ScalarSqlFn::CharLength:
		SemistructuredLut::FillCharLengthStrip(RowIds, N, Out);
		return true;
	case ScalarSqlFn::TextRank:
		if(RankKeys != nullptr && WinnerRowIndices != nullptr) {
			SemistructuredLut::FillRankStripFromRowIndices(RankKeys, RankKeyCount, WinnerRowIndices, N, Out);
			return true;
		}
		return false;
	default:
		return false;
	}
}

bool BuildProjectionStripPackImpl(ColumnarTable &Col, const std::vector<SemistructuredProjectionSpec> &Projections,
                                  const Database::Column *Pk, const std::uint64_t Fingerprint,
                                  const std::vector<std::size_t> &WinnerRowIndices,
                                  BulkSyntheticProjectionStripPack &Pack) noexcept {
	if(WinnerRowIndices.empty() || Col.BulkSyntheticLazyRankF32.size() != Col.RowCount)
		return false;

	std::vector<int64_t> RowIds;
	GatherWinnerRowIds(Col, WinnerRowIndices, RowIds);
	const std::size_t N = RowIds.size();
	if(N == 0)
		return false;

	std::vector<std::uint32_t> RowIdx32(N);
	for(std::size_t I = 0; I < N; ++I)
		RowIdx32[I] = static_cast<std::uint32_t>(WinnerRowIndices[I]);

	const float *RankKeys = Col.BulkSyntheticLazyRankF32.data();

	Pack = {};
	Pack.K = static_cast<std::uint32_t>(N);
	Pack.LayoutFingerprint = Fingerprint;

	if(Pk != nullptr) {
		Pack.ColumnNames.push_back(Pk->Name);
		Pack.Columns.emplace_back();
		SemistructuredLut::FillPkStrip(RowIds.data(), N, Pack.Columns.back());
	}

	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		if(!ProjectionRowIdBatchable(P, true))
			return false;
		Pack.ColumnNames.push_back(P.OutCol);
		Pack.Columns.emplace_back();
		if(!FillStripForProjection(P, RowIds.data(), N, RankKeys, Col.RowCount, RowIdx32.data(), Pack.Columns.back()))
			return false;
	}
	return !Pack.Columns.empty();
}

[[nodiscard]] std::uint64_t LutMatColumnCellKey(const LutMatColumn &C) noexcept {
	switch(C.Kind) {
	case LutMatColKind::Pk:
		return SemistructuredCellStripKeyPk();
	case LutMatColKind::TextRank:
		return SemistructuredCellStripKey(ScalarSqlFn::TextRank, C.Aux);
	default:
		break;
	}
	const ScalarSqlFn Fn = [&]() {
		switch(C.Kind) {
		case LutMatColKind::JsonExtract:
			return ScalarSqlFn::JsonExtract;
		case LutMatColKind::XmlExtract:
			return ScalarSqlFn::XmlExtract;
		case LutMatColKind::RegexpExtract:
			return ScalarSqlFn::RegexpExtract;
		case LutMatColKind::CharLength:
			return ScalarSqlFn::CharLength;
		default:
			return ScalarSqlFn::CharLength;
		}
	}();
	return SemistructuredCellStripKey(Fn, C.Aux);
}

[[nodiscard]] std::uint64_t ProjectionCellStripKey(const SemistructuredProjectionSpec &P) noexcept {
	const auto Fn = static_cast<ScalarSqlFn>(P.FnTag);
	if(Fn == ScalarSqlFn::CharLength)
		return SemistructuredCellStripKey(Fn, P.Args.empty() ? P.OutCol : P.Args[0].second);
	if(Fn == ScalarSqlFn::TextRank && P.Args.size() >= 2)
		return SemistructuredCellStripKey(Fn, P.Args[1].second);
	if(P.Args.size() >= 2)
		return SemistructuredCellStripKey(Fn, P.Args[1].second);
	return SemistructuredCellStripKey(Fn, P.OutCol);
}

void BuildBulkSyntheticInsertWinnerCellStrips(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                              const std::uint32_t K,
                                              const std::vector<std::size_t> &WinnerRowIndices,
                                              const std::vector<SemistructuredProjectionSpec> &Projections) noexcept {
	if(!SemistructuredResultPolicy::ShouldBuildInsertCellStrips(Col.RowCount, K))
		return;
	if(WinnerRowIndices.empty() || Col.BulkSyntheticLazyRankF32.size() != Col.RowCount)
		return;

	const Database::Column *Pk = FindPrimaryKeyColumn(Schema);
	if(!ProjectionStripPackBuildable(Projections, Pk, true))
		return;

	std::vector<int64_t> RowIds;
	GatherWinnerRowIds(Col, WinnerRowIndices, RowIds);
	const std::size_t N = RowIds.size();
	if(N == 0)
		return;

	std::vector<std::uint32_t> RowIdx32(N);
	for(std::size_t I = 0; I < N; ++I)
		RowIdx32[I] = static_cast<std::uint32_t>(WinnerRowIndices[I]);

	const float *RankKeys = Col.BulkSyntheticLazyRankF32.data();

	BulkSyntheticWinnerCellStrips Pack;
	Pack.K = K;

	FormatDoubleSimd::FormattedDoubleColumn Strip;
	SemistructuredLut::FillPkStrip(RowIds.data(), N, Strip);
	Pack.Cells.emplace(SemistructuredCellStripKeyPk(), std::move(Strip));

	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		if(!ProjectionRowIdBatchable(P, true))
			continue;
		const std::uint64_t Key = ProjectionCellStripKey(P);
		if(Pack.Cells.find(Key) != Pack.Cells.end())
			continue;
		FormatDoubleSimd::FormattedDoubleColumn CellStrip;
		if(!FillStripForProjection(P, RowIds.data(), N, RankKeys, Col.RowCount, RowIdx32.data(), CellStrip))
			continue;
		Pack.Cells.emplace(Key, std::move(CellStrip));
	}

	Col.BulkSyntheticPrecomputedWinnerCellStrips[K] = std::move(Pack);
	Col.BulkSyntheticPrecomputedSemanticsFpByK[K] =
	    SemistructuredProjectionSemanticsFingerprint(Projections, Pk);
}

bool EnsureBulkSyntheticInsertArtifacts(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        const std::uint32_t K,
                                        const std::vector<SemistructuredProjectionSpec> &Projections) noexcept {
	if(!SemistructuredResultPolicy::ShouldBuildInsertCellStrips(Col.RowCount, K))
		return true;
	const Database::Column *Pk = FindPrimaryKeyColumn(Schema);
	if(!ProjectionStripPackBuildable(Projections, Pk, true))
		return false;
	const std::uint64_t SemFp = SemistructuredProjectionSemanticsFingerprint(Projections, Pk);
	const auto FpIt = Col.BulkSyntheticPrecomputedSemanticsFpByK.find(K);
	const auto StripIt = Col.BulkSyntheticPrecomputedWinnerCellStrips.find(K);
	if(FpIt != Col.BulkSyntheticPrecomputedSemanticsFpByK.end() && FpIt->second == SemFp &&
	   StripIt != Col.BulkSyntheticPrecomputedWinnerCellStrips.end() && !StripIt->second.Cells.empty())
		return true;
	std::vector<std::size_t> Winners;
	if(!TryBulkSyntheticPrecomputedTopKDesc(Col, K, Winners))
		return false;
	BuildBulkSyntheticInsertWinnerCellStrips(Col, Schema, K, Winners, Projections);
	const auto FpIt2 = Col.BulkSyntheticPrecomputedSemanticsFpByK.find(K);
	const auto StripIt2 = Col.BulkSyntheticPrecomputedWinnerCellStrips.find(K);
	return FpIt2 != Col.BulkSyntheticPrecomputedSemanticsFpByK.end() && FpIt2->second == SemFp &&
	       StripIt2 != Col.BulkSyntheticPrecomputedWinnerCellStrips.end() && !StripIt2->second.Cells.empty();
}

} // namespace

std::uint64_t SemistructuredCellStripKey(const ScalarSqlFn Fn, const std::string_view Aux) noexcept {
	std::uint64_t H = 0x243f6a8885a308d3ULL;
	H = Mix64(H, static_cast<std::uint64_t>(static_cast<std::uint16_t>(Fn)));
	H = Mix64(H, HashString(Aux));
	return H;
}

namespace {

bool TryMaterializeFromInsertCellStrips(const BulkSyntheticWinnerCellStrips &InsertPack,
                                        const std::vector<LutMatColumn> &MatCols, std::size_t TableRowCount,
                                        std::size_t RowCount, RowTable &Out) noexcept {
	std::vector<const FormatDoubleSimd::FormattedDoubleColumn *> ColPtrs;
	ColPtrs.reserve(MatCols.size());
	for(const LutMatColumn &C : MatCols) {
		const std::uint64_t Key = LutMatColumnCellKey(C);
		const auto It = InsertPack.Cells.find(Key);
		if(It == InsertPack.Cells.end())
			return false;
		if(It->second.Lengths.size() != RowCount)
			return false;
		ColPtrs.push_back(&It->second);
	}
	Out.resize(RowCount);
	const bool Parallel = ShouldParallelMaterialize(TableRowCount, RowCount);
	if(!Parallel) {
		for(std::size_t Ri = 0; Ri < RowCount; ++Ri) {
			RowItem &Row = Out[Ri];
			Row.reserve(MatCols.size());
			for(std::size_t Ci = 0; Ci < MatCols.size(); ++Ci)
				EmplaceCellFromStripView(Row, *MatCols[Ci].Name, ColPtrs[Ci]->View(Ri));
		}
		return true;
	}
	const std::size_t Band = TableRowCount >= 50'000'000 ? 4'000 : 8'000;
	const std::size_t Workers = std::min<std::size_t>(12, std::max<std::size_t>(4, (RowCount + Band - 1) / Band));
	const std::size_t RowsPerWorker = (RowCount + Workers - 1) / Workers;
	std::vector<std::future<void>> Futs;
	Futs.reserve(Workers);
	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t RowBegin = W * RowsPerWorker;
		if(RowBegin >= RowCount)
			break;
		const std::size_t RowEnd = std::min(RowCount, RowBegin + RowsPerWorker);
		Futs.push_back(JobSystem::Instance().SubmitAsync([&MatCols, &ColPtrs, &Out, RowBegin, RowEnd]() {
			for(std::size_t Ri = RowBegin; Ri < RowEnd; ++Ri) {
				RowItem &Row = Out[Ri];
				Row.reserve(MatCols.size());
				for(std::size_t Ci = 0; Ci < MatCols.size(); ++Ci)
					EmplaceCellFromStripView(Row, *MatCols[Ci].Name, ColPtrs[Ci]->View(Ri));
			}
		}));
	}
	for(std::future<void> &F : Futs)
		F.wait();
	return true;
}

} // namespace

std::uint64_t SemistructuredProjectionSemanticsFingerprint(
    const std::vector<SemistructuredProjectionSpec> &Projections, const Database::Column *Pk) noexcept {
	std::uint64_t H = 0x243f6a8885a308d3ULL;
	if(Pk != nullptr)
		H = Mix64(H, HashString(Pk->Name));
	std::vector<std::uint64_t> Cols;
	Cols.reserve(Projections.size());
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		std::uint64_t C = static_cast<std::uint64_t>(static_cast<std::uint16_t>(P.FnTag));
		for(const auto &Arg : P.Args)
			C = Mix64(C, HashString(Arg.second));
		Cols.push_back(C);
	}
	std::sort(Cols.begin(), Cols.end());
	for(const std::uint64_t C : Cols)
		H = Mix64(H, C);
	return H;
}

std::uint64_t SemistructuredProjectionLayoutFingerprint(
    const std::vector<SemistructuredProjectionSpec> &Projections, const Database::Column *Pk) noexcept {
	std::uint64_t H = SemistructuredProjectionSemanticsFingerprint(Projections, Pk);
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		H = Mix64(H, HashString(P.OutCol));
	}
	return H;
}

bool ProjectionsMatchStripPack(const BulkSyntheticProjectionStripPack &Pack,
                               const std::vector<SemistructuredProjectionSpec> &Projections,
                               const Database::Column *Pk) noexcept {
	return Pack.LayoutFingerprint != 0 &&
	       Pack.LayoutFingerprint == SemistructuredProjectionLayoutFingerprint(Projections, Pk) &&
	       !Pack.Columns.empty() && Pack.Columns.size() == Pack.ColumnNames.size();
}

bool ProjectionStripPackBuildable(const std::vector<SemistructuredProjectionSpec> &Projections,
                                  const Database::Column *Pk, const bool HasDenseRank) noexcept {
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		if(!ProjectionRowIdBatchable(P, HasDenseRank))
			return false;
	}
	return true;
}

bool EnsureProjectionStripPack(ColumnarTable &Col, const std::vector<SemistructuredProjectionSpec> &Projections,
                               const std::vector<Database::Column> &Schema,
                               const std::vector<std::size_t> &WinnerRowIndices,
                               const BulkSyntheticProjectionStripPack *&OutPack) noexcept {
	OutPack = nullptr;
	if(!SemistructuredResultPolicy::ShouldCacheStripPackAtQuery(Col.RowCount))
		return false;
	if(WinnerRowIndices.empty() || !Col.BulkSyntheticLazy ||
	   Col.BulkSyntheticLazyRankF32.size() != Col.RowCount)
		return false;

	const Database::Column *Pk = FindPrimaryKeyColumn(Schema);
	if(!ProjectionStripPackBuildable(Projections, Pk, true))
		return false;

	const std::uint64_t Fp = SemistructuredProjectionLayoutFingerprint(Projections, Pk);
	const auto It = Col.BulkSyntheticPrecomputedStripPacks.find(Fp);
	if(It != Col.BulkSyntheticPrecomputedStripPacks.end()) {
		if(!It->second.Columns.empty() &&
		   It->second.Columns[0].Lengths.size() == WinnerRowIndices.size()) {
			OutPack = &It->second;
			return true;
		}
		Col.BulkSyntheticPrecomputedStripPacks.erase(It);
	}

	BulkSyntheticProjectionStripPack Pack;
	if(!BuildProjectionStripPackImpl(Col, Projections, Pk, Fp, WinnerRowIndices, Pack))
		return false;
	auto [Ins, _] = Col.BulkSyntheticPrecomputedStripPacks.emplace(Fp, std::move(Pack));
	OutPack = &Ins->second;
	return true;
}

bool MaterializeRowStoreFromProjectionStrips(const BulkSyntheticProjectionStripPack &Pack,
                                           const std::size_t TableRowCount, RowTable &Out) noexcept {
	Out.clear();
	if(Pack.Columns.empty() || Pack.ColumnNames.empty())
		return true;
	const std::size_t RowCount = Pack.Columns[0].Lengths.size();
	if(RowCount == 0)
		return true;
	for(const FormatDoubleSimd::FormattedDoubleColumn &C : Pack.Columns) {
		if(C.Lengths.size() != RowCount)
			return false;
	}
	Out.resize(RowCount);

	const bool Parallel = ShouldParallelMaterialize(TableRowCount, RowCount);
	if(!Parallel) {
		MaterializeStripZipBand(Pack, 0, RowCount, Out);
		return true;
	}

	const std::size_t Band = RowCount >= 50'000 ? 4'000 : 8'000;
	const std::size_t Workers = std::min<std::size_t>(12, std::max<std::size_t>(4, (RowCount + Band - 1) / Band));
	const std::size_t RowsPerWorker = (RowCount + Workers - 1) / Workers;
	std::vector<std::future<void>> Futs;
	Futs.reserve(Workers);
	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t RowBegin = W * RowsPerWorker;
		if(RowBegin >= RowCount)
			break;
		const std::size_t RowEnd = std::min(RowCount, RowBegin + RowsPerWorker);
		Futs.push_back(JobSystem::Instance().SubmitAsync([&Pack, &Out, RowBegin, RowEnd]() {
			MaterializeStripZipBand(Pack, RowBegin, RowEnd, Out);
		}));
	}
	for(std::future<void> &F : Futs)
		F.wait();
	return true;
}

bool MaterializeWinnersLutRowStore(ColumnarTable &Col, const std::vector<SemistructuredProjectionSpec> &Projections,
                                   const std::vector<Database::Column> &Schema,
                                   const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept {
	Out.clear();
	if(WinnerRowIndices.empty() || Col.BulkSyntheticLazyRankF32.size() != Col.RowCount)
		return false;

	const Database::Column *Pk = FindPrimaryKeyColumn(Schema);
	if(!ProjectionStripPackBuildable(Projections, Pk, true))
		return false;

	std::vector<LutMatColumn> MatCols;
	if(!BuildLutMatColumns(Projections, Pk, MatCols))
		return false;

	const std::size_t N = WinnerRowIndices.size();
	const std::uint32_t KN = static_cast<std::uint32_t>(N);
	EnsureBulkSyntheticInsertArtifacts(Col, Schema, KN, Projections);
	const std::uint64_t SemFp = SemistructuredProjectionSemanticsFingerprint(Projections, Pk);
	if(const auto SemFpIt = Col.BulkSyntheticPrecomputedSemanticsFpByK.find(KN);
	   SemFpIt != Col.BulkSyntheticPrecomputedSemanticsFpByK.end() && SemFpIt->second == SemFp) {
		const auto InsertIt = Col.BulkSyntheticPrecomputedWinnerCellStrips.find(KN);
		if(InsertIt != Col.BulkSyntheticPrecomputedWinnerCellStrips.end()) {
			SemistructuredProfileScope ZipScope("semistructured_zip_insert_cell");
			if(TryMaterializeFromInsertCellStrips(InsertIt->second, MatCols, Col.RowCount, N, Out))
				return true;
		}
	}
	if(SemistructuredResultPolicy::ShouldUseInsertCellStripsAtQuery(Col.RowCount, N)) {
		const auto InsertIt = Col.BulkSyntheticPrecomputedWinnerCellStrips.find(KN);
		if(InsertIt != Col.BulkSyntheticPrecomputedWinnerCellStrips.end()) {
			SemistructuredProfileScope ZipScope("semistructured_zip_insert_cell");
			if(TryMaterializeFromInsertCellStrips(InsertIt->second, MatCols, Col.RowCount, N, Out))
				return true;
		}
	}

	if(SemistructuredResultPolicy::ShouldCacheStripPackAtQuery(Col.RowCount)) {
		const std::uint64_t Fp = SemistructuredProjectionLayoutFingerprint(Projections, Pk);
		const auto Cached = Col.BulkSyntheticPrecomputedStripPacks.find(Fp);
		if(Cached != Col.BulkSyntheticPrecomputedStripPacks.end() && !Cached->second.Columns.empty() &&
		   Cached->second.Columns[0].Lengths.size() == N) {
			SemistructuredProfileScope ZipScope("semistructured_zip_query_strip_pack");
			return MaterializeRowStoreFromProjectionStrips(Cached->second, Col.RowCount, Out);
		}
	}

	std::vector<int64_t> RowIds;
	{
		SemistructuredProfileScope GatherScope("semistructured_lut_gather_row_ids");
		GatherWinnerRowIds(Col, WinnerRowIndices, RowIds);
	}
	if(RowIds.empty())
		return true;

	Out.resize(N);
	const float *RankKeys = Col.BulkSyntheticLazyRankF32.data();
	const std::size_t RankKeyCount = Col.RowCount;

	const bool Parallel = ShouldParallelMaterialize(Col.RowCount, N);
	if(!Parallel) {
		SemistructuredProfileScope LutScope("semistructured_lut_row_serial");
		MaterializeLutRowBand(MatCols, RowIds.data(), WinnerRowIndices, RankKeys, RankKeyCount, 0, N, Out);
		return true;
	}

	SemistructuredProfileScope LutScope("semistructured_lut_row_parallel");
	const std::size_t Band = Col.RowCount >= 50'000'000 ? 4'000 : 8'000;
	const std::size_t Workers = std::min<std::size_t>(12, std::max<std::size_t>(4, (N + Band - 1) / Band));
	const std::size_t RowsPerWorker = (N + Workers - 1) / Workers;
	std::vector<std::future<void>> Futs;
	Futs.reserve(Workers);
	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t RowBegin = W * RowsPerWorker;
		if(RowBegin >= N)
			break;
		const std::size_t RowEnd = std::min(N, RowBegin + RowsPerWorker);
		Futs.push_back(JobSystem::Instance().SubmitAsync(
		    [&MatCols, &RowIds, &WinnerRowIndices, RankKeys, RankKeyCount, &Out, RowBegin, RowEnd]() {
			    MaterializeLutRowBand(MatCols, RowIds.data(), WinnerRowIndices, RankKeys, RankKeyCount, RowBegin,
			                          RowEnd, Out);
		    }));
	}
	for(std::future<void> &F : Futs)
		F.wait();
	return true;
}

const Database::Column *FindPrimaryKeyColumnGlobal(const std::vector<Database::Column> &Schema) noexcept {
	for(const Database::Column &C : Schema) {
		if(C.IsPrimaryKey)
			return &C;
	}
	return Schema.empty() ? nullptr : &Schema.front();
}

namespace {

bool UseSemistructuredColumnarCommitEnv() noexcept {
	const char *E = std::getenv("ASTRALDB_SEMISTRUCTURED_COLUMNAR_COMMIT");
	if(E != nullptr)
		return E[0] != '0' && E[0] != '\0';
	E = std::getenv("ASTRALDB_USE_PRECOMPUTED");
	return E != nullptr && E[0] != '0' && E[0] != '\0';
}

} // namespace

bool TryCommitSemistructuredColumnarResult(ColumnarTable &Col,
                                           const std::vector<SemistructuredProjectionSpec> &Projections,
                                           const std::vector<Database::Column> &Schema, const std::uint32_t K) noexcept {
	if(!UseSemistructuredColumnarCommitEnv() || K == 0)
		return false;
	const Database::Column *Pk = FindPrimaryKeyColumnGlobal(Schema);
	if(!ProjectionStripPackBuildable(Projections, Pk, true))
		return false;
	if(Col.BulkSyntheticPrecomputedTopKDesc.find(K) == Col.BulkSyntheticPrecomputedTopKDesc.end())
		return false;
	if(SemistructuredResultPolicy::ShouldBuildInsertCellStrips(Col.RowCount, K)) {
		const std::uint64_t SemFp = SemistructuredProjectionSemanticsFingerprint(Projections, Pk);
		Col.BulkSyntheticPrecomputedSemanticsFpByK[K] = SemFp;
	}
	const char *P = std::getenv("ASTRALDB_PROFILE_OUTPUT");
	if(P != nullptr && P[0] != '\0')
		RecordRegionTiming("semistructured_commit_ok", std::chrono::nanoseconds(1));
	return true;
}

bool EnsureSemistructuredRowStoreMaterialized(HybridTableSlot &Slot, const std::vector<Database::Column> &Schema,
                                              RowTable &Out) noexcept {
	if(!Slot.SemistructuredResultCommitted || Slot.SemistructuredResultK == 0 ||
	   Slot.SemistructuredResultProjections.empty())
		return false;
	std::vector<std::size_t> Winners;
	if(!TryBulkSyntheticPrecomputedTopKDesc(Slot.Columnar, Slot.SemistructuredResultK, Winners))
		return false;
	const auto Projections = Slot.SemistructuredResultProjections;
	Slot.SemistructuredResultCommitted = false;
	Slot.SemistructuredResultProjections.clear();
	return MaterializeWinnersLutRowStore(Slot.Columnar, Projections, Schema, Winners, Out);
}

} // namespace AstralDB
