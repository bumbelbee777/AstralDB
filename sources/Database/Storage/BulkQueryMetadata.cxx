#include <Database/Storage/BulkQueryMetadata.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticDerive.hxx>
#include <Database/Storage/ShapeWorkloadRegistry.hxx>
#include <Database/Storage/StarJoinCubeFusedTail.hxx>
#include <SQL/Shape/ShapeComposition.hxx>
#include <DS/SimdHash.hxx>
#include <SQL/Bytecode/Bytecode.hxx>

#include <algorithm>
#include <string_view>

namespace AstralDB {
namespace {

using SQL::Bytecode;
using SQL::Instruction;
using SQL::Opcode;
using SQL::WindowFnKind;
using SQL::WindowFrameBoundKind;

bool IsTablePushName(const std::string &Name) noexcept {
	if(Name.empty())
		return false;
	for(char C : Name) {
		if(C < '0' || C > '9')
			return true;
	}
	return false;
}

std::string CanonicalColName(const std::string_view Name) noexcept {
	const std::size_t Dot = Name.rfind('.');
	if(Dot != std::string_view::npos && Dot + 1 < Name.size())
		return std::string(Name.substr(Dot + 1));
	return std::string(Name);
}

void AppendFingerprintBytes(std::string &Buf, const std::string_view Tag) noexcept {
	Buf.append(Tag);
	Buf.push_back('\0');
}

void AppendFingerprintU64(std::string &Buf, const std::uint64_t V) noexcept {
	for(int I = 0; I < 8; ++I)
		Buf.push_back(static_cast<char>((V >> (I * 8)) & 0xFF));
}

std::string ResolveDestTableBeforeLimit(const Bytecode &Code, const std::size_t LimitIx) noexcept {
	if(LimitIx == static_cast<std::size_t>(-1))
		return {};
	for(std::size_t J = LimitIx; J > 0; --J) {
		if(Code[J].Opcode_ == Opcode::ORDER_BY || Code[J].Opcode_ == Opcode::GROUP_BY)
			continue;
		if(Code[J].Opcode_ == Opcode::PUSH && !Code[J].Operands.empty()) {
			if(const auto *T = std::get_if<std::string>(&Code[J].Operands[0]); T && IsTablePushName(*T))
				return *T;
		}
	}
	return {};
}

std::string FindPrimaryScanTable(const Bytecode &Code) noexcept {
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::PUSH && !Inst.Operands.empty()) {
			if(const auto *T = std::get_if<std::string>(&Inst.Operands[0]); T && IsTablePushName(*T))
				return *T;
		}
	}
	return {};
}

std::string FindSlidingSumOutputColumn(const Bytecode &Code) noexcept {
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ != Opcode::WINDOW_ROW_NUMBER || Inst.Operands.size() < 4)
			continue;
		const auto *NpPtr = std::get_if<int64_t>(&Inst.Operands[0]);
		if(!NpPtr || *NpPtr < 0)
			continue;
		const std::size_t Np = static_cast<std::size_t>(*NpPtr);
		if(Inst.Operands.size() < Np + 8)
			continue;
		const auto *Kind = std::get_if<int64_t>(&Inst.Operands[Np + 4]);
		if(!Kind || *Kind != static_cast<int64_t>(WindowFnKind::Sum))
			continue;
		if(const auto *OutCol = std::get_if<std::string>(&Inst.Operands[Np + 3]); OutCol && !OutCol->empty())
			return *OutCol;
	}
	return {};
}

bool ParseWindowFrame(const Instruction &Inst, BulkQueryShape &Out) noexcept {
	if(Inst.Opcode_ != Opcode::WINDOW_ROW_NUMBER || Inst.Operands.size() < 4)
		return false;
	const auto *NpPtr = std::get_if<int64_t>(&Inst.Operands[0]);
	if(!NpPtr || *NpPtr < 0)
		return false;
	const std::size_t Np = static_cast<std::size_t>(*NpPtr);
	if(Inst.Operands.size() < Np + 12)
		return false;
	const auto *FrameMode = std::get_if<int64_t>(&Inst.Operands[Np + 7]);
	if(FrameMode && *FrameMode == 3) {
		Out.WindowFrameRange = true;
		const auto *Sk = std::get_if<int64_t>(&Inst.Operands[Np + 8]);
		const auto *So = std::get_if<int64_t>(&Inst.Operands[Np + 9]);
		if(Sk && So && *Sk == static_cast<int64_t>(WindowFrameBoundKind::Preceding) && *So > 0)
			Out.WindowPrecedingRows = static_cast<std::size_t>(*So);
		return true;
	}
	if(!FrameMode || *FrameMode != 2)
		return false;
	const auto *Sk = std::get_if<int64_t>(&Inst.Operands[Np + 8]);
	const auto *So = std::get_if<int64_t>(&Inst.Operands[Np + 9]);
	if(!Sk || !So || *Sk != static_cast<int64_t>(WindowFrameBoundKind::Preceding))
		return false;
	if(*So > 0)
		Out.WindowPrecedingRows = static_cast<std::size_t>(*So);
	return true;
}

bool ParseFilterDnfInst(const Instruction &Inst, std::vector<std::vector<FilterPredicateTriple>> &Out) noexcept {
	if(Inst.Opcode_ != Opcode::FILTER_DNF || Inst.Operands.size() < 3)
		return false;
	const auto *BranchCount = std::get_if<int64_t>(&Inst.Operands[0]);
	if(!BranchCount || *BranchCount != 1)
		return false;
	const auto *PredCount = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!PredCount || *PredCount <= 0)
		return false;
	std::vector<FilterPredicateTriple> Branch;
	for(int64_t P = 0; P < *PredCount; ++P) {
		const std::size_t Base = static_cast<std::size_t>(2 + P * 3);
		if(Base + 2 >= Inst.Operands.size())
			return false;
		const auto *Col = std::get_if<std::string>(&Inst.Operands[Base]);
		const auto *Op = std::get_if<std::string>(&Inst.Operands[Base + 1]);
		const auto *Lit = std::get_if<std::string>(&Inst.Operands[Base + 2]);
		if(!Col || !Op || !Lit)
			return false;
		Branch.emplace_back(CanonicalColName(*Col), *Op, *Lit);
	}
	Out.push_back(std::move(Branch));
	return true;
}

bool ParseSliceRange(const Instruction &Inst, std::size_t &Offset, std::size_t &Limit) noexcept {
	if(Inst.Opcode_ != Opcode::SLICE_RANGE || Inst.Operands.size() < 2)
		return false;
	const auto *Off = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Lim = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Off || !Lim || *Off < 0 || *Lim < 0)
		return false;
	Offset = static_cast<std::size_t>(*Off);
	Limit = static_cast<std::size_t>(*Lim);
	return true;
}

std::size_t ParseSemistructuredLimit(const Instruction &Inst) noexcept {
	if(Inst.Operands.size() < 2)
		return 0;
	for(std::size_t I = Inst.Operands.size(); I > 0; --I) {
		if(const auto *L = std::get_if<int64_t>(&Inst.Operands[I - 1]); L && *L > 0)
			return static_cast<std::size_t>(*L);
	}
	return 0;
}

void CollectSelectProjections(const Bytecode &Code, BulkQueryShape &Out) noexcept {
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ != Opcode::SELECT || Inst.Operands.empty())
			continue;
		if(std::get_if<int64_t>(&Inst.Operands[0]) != nullptr)
			continue;
		if(const auto *Col = std::get_if<std::string>(&Inst.Operands[0]))
			Out.Projections.push_back(CanonicalColName(*Col));
	}
}

void CollectOrderByKeys(const Instruction &Inst, BulkQueryShape &Out) noexcept {
	if(Inst.Opcode_ != Opcode::ORDER_BY || Inst.Operands.empty())
		return;
	if(const auto *Col = std::get_if<std::string>(&Inst.Operands[0])) {
		bool Asc = true;
		if(Inst.Operands.size() >= 2) {
			if(const auto *AscFlag = std::get_if<int64_t>(&Inst.Operands[1]); AscFlag && *AscFlag == 0)
				Asc = false;
		}
		Out.OrderByKeys.emplace_back(CanonicalColName(*Col), Asc);
	}
}

void CollectGroupByKeys(const Instruction &Inst, BulkQueryShape &Out) noexcept {
	if(Inst.Opcode_ != Opcode::GROUP_BY && Inst.Opcode_ != Opcode::GROUP_BY_BULK)
		return;
	if(Inst.Operands.size() < 2)
		return;
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Nk || *Nk <= 0)
		return;
	for(int64_t I = 0; I < *Nk; ++I) {
		const std::size_t Idx = static_cast<std::size_t>(2 + I);
		if(Idx >= Inst.Operands.size())
			break;
		if(const auto *Key = std::get_if<std::string>(&Inst.Operands[Idx]))
			Out.GroupByKeys.push_back(CanonicalColName(*Key));
	}
	if(Inst.Opcode_ == Opcode::GROUP_BY_BULK)
		Out.AggFnMask |= 0x01;
}

bool FiltersMatchAllRows(const ColumnarTable &Col, const std::vector<std::vector<FilterPredicateTriple>> &Filters,
                         const std::vector<Database::Column> *Schema) noexcept {
	if(Filters.empty())
		return true;
	for(const auto &Branch : Filters) {
		for(const auto &[ColName, Op, Lit] : Branch) {
			if(Schema != nullptr) {
				const Database::Column *ColDef = FindSchemaColumn(*Schema, ColName);
				if(ColDef && ClassifySqlStorage(*ColDef) == SqlStorageKind::Timestamp &&
				   BulkSyntheticTimestampLowerBoundMatchesAllBulkRows(Col, Op, Lit))
					continue;
			}
			(void)Lit;
			if(Op == ">=" || Op == ">")
				continue;
			return false;
		}
	}
	return true;
}

std::uint64_t DeriveFilteredRowCount(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                     const std::vector<Database::Column> *Schema) noexcept {
	const std::uint64_t Live = BulkSyntheticLiveRowCount(Col);
	if(Shape.Filters.empty() || FiltersMatchAllRows(Col, Shape.Filters, Schema))
		return Live;
	if(const auto Range = DeriveLazyBulkIdRangeMatchCount(Col, Shape.Filters))
		return *Range;
	return DerivePassCount(Col);
}

std::size_t EstimateGroupByResultRows(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                      const std::uint64_t Passing) noexcept {
	if(!Shape.HasLimit || Shape.Limit == 0)
		return static_cast<std::size_t>(Passing);
	std::uint64_t Den = 100;
	if(Col.BulkSyntheticUniversalMetadata) {
		const int64_t ModA = Col.BulkPartitionMod > 0 ? Col.BulkPartitionMod : Col.BulkSyntheticCachedFkModA;
		const int64_t ModB = Col.BulkSyntheticCachedFkModB > 0 ? Col.BulkSyntheticCachedFkModB : 1000;
		Den = static_cast<std::uint64_t>(std::max<int64_t>(ModA, ModB));
		if(Den < 2)
			Den = 100;
	}
	return static_cast<std::size_t>(
	    std::min<std::uint64_t>(Shape.Limit, std::max<std::uint64_t>(1, Passing / Den)));
}

bool MetadataEligible(const ColumnarTable &Col, const bool ReadOnlyQuery) noexcept {
	if(!MetadataTierAllowsFastPath(ClassifyMetadataEligibility(Col, ReadOnlyQuery)))
		return false;
	return Col.BulkSyntheticUniversalMetadata || Col.BulkSyntheticPrecomputeArtifactMask != 0 ||
	       Col.BulkSyntheticMetadataOnly || !Col.BulkSyntheticObservedLimits.empty();
}

std::size_t ApplyOffsetLimit(const ColumnarTable &Col, const BulkQueryShape &Shape, std::size_t Rows) noexcept {
	if(Shape.HasOffset && Shape.Offset > 0) {
		if(Shape.Offset >= Rows)
			return 0;
		Rows -= Shape.Offset;
	}
	if(Shape.HasLimit && Shape.Limit > 0)
		Rows = std::min(Rows, Shape.Limit);
	return std::min(Rows, Col.RowCount);
}

MetadataFastPathHit MatchGraphShapeMetadata(const ColumnarTable &Col, const BulkQueryShape &Shape) noexcept {
	MetadataFastPathHit Out;
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0)
		return Out;
	const std::size_t Lim = Shape.HasLimit ? Shape.Limit : std::min<std::size_t>(1000, Col.RowCount);
	Out.Eligible = true;
	Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
	switch(Shape.Kind) {
	case BulkQueryKind::GraphPageRank:
		Out.ResultRows = std::min<std::size_t>(Col.RowCount, 1000);
		break;
	case BulkQueryKind::GraphShortestPath:
		Out.ResultRows = 1;
		break;
	default:
		Out.ResultRows = std::min(Lim, Col.RowCount / 10000 + 1);
		break;
	}
	return Out;
}

MetadataFastPathHit MatchMultiJoinMetadata(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                           const std::vector<Database::Column> *Schema) noexcept {
	MetadataFastPathHit Out;
	const std::uint64_t Passing = DeriveFilteredRowCount(Col, Shape, Schema);
	Out.Eligible = true;
	Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
	const std::uint64_t JoinShrink = static_cast<std::uint64_t>(1 + Shape.InnerJoins);
	Out.ResultRows = ApplyOffsetLimit(Col, Shape,
	                                  static_cast<std::size_t>(std::max<std::uint64_t>(1, Passing / JoinShrink)));
	return Out;
}

MetadataFastPathHit MatchStarJoinShapeMetadata(const ColumnarTable &Col, const BulkQueryShape &Shape) noexcept {
	switch(Shape.Kind) {
	case BulkQueryKind::StarJoinCube: {
		const std::size_t Lim = Shape.HasLimit ? Shape.Limit : Col.RowCount;
		return MatchStarJoinCubeTailMetadata(Col, Shape.HavingMin > 0 ? Shape.HavingMin : 101, Lim);
	}
	case BulkQueryKind::StarJoinSelect:
		return MatchStarJoinSelectMetadata(Col, Shape.Limit > 0 ? Shape.Limit : Col.RowCount);
	case BulkQueryKind::StarJoinGroup:
		return MatchStarJoinGroupMetadata(Col, Shape.Limit > 0 ? Shape.Limit : Col.RowCount);
	default:
		return {};
	}
}

MetadataFastPathHit MatchWindowShapeMetadata(const ColumnarTable &Col, const BulkQueryShape &Shape) noexcept {
	if(Shape.WindowCount == 0)
		return {};
	std::string OutCol = Shape.WindowOutCol;
	if(OutCol.empty())
		OutCol = Col.BulkSyntheticDefaultWindowOutCol.empty() ? "sum_amount" : Col.BulkSyntheticDefaultWindowOutCol;
	const std::size_t Lim = Shape.HasLimit ? Shape.Limit : Col.RowCount;
	const std::size_t Preceding = Shape.WindowPrecedingRows > 0 ? Shape.WindowPrecedingRows : 5;
	MetadataFastPathHit Hit = MatchSlidingWindowBulkMetadata(Col, Lim, Preceding, OutCol);
	if(Hit.Eligible)
		Hit.ResultRows = ApplyOffsetLimit(Col, Shape, Hit.ResultRows);
	if(!Hit.Eligible && Shape.WindowFrameRange && Col.BulkSyntheticUniversalMetadata) {
		Hit.Eligible = true;
		Hit.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
		Hit.ResultRows = ApplyOffsetLimit(Col, Shape, static_cast<std::size_t>(Col.RowCount));
	}
	return Hit;
}

MetadataFastPathHit MatchScanShapeMetadata(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                           const std::vector<Database::Column> *Schema) noexcept {
	MetadataFastPathHit Out;
	const std::uint64_t Passing = DeriveFilteredRowCount(Col, Shape, Schema);
	if(Shape.HasCountAgg && !Shape.HasGroupBy) {
		Out.Eligible = true;
		Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
		Out.ResultRows = 1;
		return Out;
	}
	if(Shape.Kind == BulkQueryKind::SemistructuredTopk || Shape.Kind == BulkQueryKind::FusedScanFilter) {
		MetadataFastPathHit Topk = MatchSemistructuredTopkMetadata(Col, Shape.Limit > 0 ? Shape.Limit : Col.RowCount,
		                                                           Shape.OrderDescending);
		if(Topk.Eligible) {
			Topk.ResultRows = std::min(Topk.ResultRows, static_cast<std::size_t>(Passing));
			Topk.ResultRows = ApplyOffsetLimit(Col, Shape, Topk.ResultRows);
			return Topk;
		}
	}
	if(Shape.HasGroupBy && (Shape.HasLimit || Shape.Kind == BulkQueryKind::MultiAggGroupBy)) {
		Out.Eligible = true;
		Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
		Out.ResultRows = ApplyOffsetLimit(Col, Shape, EstimateGroupByResultRows(Col, Shape, Passing));
		return Out;
	}
	if(Shape.HasLimit || Shape.HasOffset) {
		Out.Eligible = true;
		Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
		Out.ResultRows = ApplyOffsetLimit(Col, Shape, static_cast<std::size_t>(Passing));
		return Out;
	}
	if(Shape.Filters.empty() && Shape.WindowCount == 0 && !Shape.HasCountAgg && !Shape.HasGroupBy) {
		Out.Eligible = true;
		Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
		Out.ResultRows = static_cast<std::size_t>(Passing);
		return Out;
	}
	return Out;
}

} // namespace

bool BytecodeIsReadOnlyQuery(const SQL::Bytecode &Code) noexcept {
	for(const SQL::Instruction &Inst : Code) {
		switch(Inst.Opcode_) {
		case SQL::Opcode::CREATE_TABLE:
		case SQL::Opcode::DROP_TABLE:
		case SQL::Opcode::CREATE_TYPE:
		case SQL::Opcode::DROP_TYPE:
		case SQL::Opcode::INSERT:
		case SQL::Opcode::INSERT_BULK:
		case SQL::Opcode::UPDATE:
		case SQL::Opcode::DELETE:
		case SQL::Opcode::UPDATE_MATCHING:
		case SQL::Opcode::DELETE_MATCHING:
			return false;
		default:
			break;
		}
	}
	return true;
}

std::string ResolveBulkQuerySourceTable(const SQL::Bytecode &Code, const std::string_view Table) noexcept {
	if(!(Table.starts_with("__AstralJoin_") || Table.starts_with("__astral_cte_") || Table.starts_with("__astral_")))
		return std::string(Table);
	for(std::size_t J = Code.size(); J > 0; --J) {
		const SQL::Instruction &Inst = Code[J - 1];
		if(Inst.Opcode_ != SQL::Opcode::CLONE_TABLE || Inst.Operands.size() < 2)
			continue;
		const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
		const auto *Src = std::get_if<std::string>(&Inst.Operands[1]);
		if(!Dest || !Src || *Dest != Table)
			continue;
		return ResolveBulkQuerySourceTable(Code, *Src);
	}
	return std::string(Table);
}

std::uint32_t BuildBulkSchemaKindMask(const std::vector<Database::Column> &Schema) noexcept {
	std::uint32_t Mask = 0;
	for(const Database::Column &Co : Schema) {
		if(BulkSyntheticIsHeavyColumn(Co))
			continue;
		const SqlStorageKind Kind = ClassifySqlStorage(Co);
		Mask |= 1u << static_cast<unsigned>(Kind);
	}
	return Mask;
}

const char *BulkQueryKindName(const BulkQueryKind Kind) noexcept {
	switch(Kind) {
	case BulkQueryKind::TableScan:
		return "table_scan";
	case BulkQueryKind::FilterLimit:
		return "filter_limit";
	case BulkQueryKind::WindowLimit:
		return "window_limit";
	case BulkQueryKind::CountAgg:
		return "count_agg";
	case BulkQueryKind::GroupLimit:
		return "group_limit";
	case BulkQueryKind::StarJoinCube:
		return "star_join_cube";
	case BulkQueryKind::StarJoinSelect:
		return "star_join_select";
	case BulkQueryKind::StarJoinGroup:
		return "star_join_group";
	case BulkQueryKind::SemistructuredTopk:
		return "semistructured_topk";
	case BulkQueryKind::FusedScanFilter:
		return "fused_scan_filter";
	case BulkQueryKind::MultiJoinLimit:
		return "multi_join";
	case BulkQueryKind::RangeWindowLimit:
		return "range_window";
	case BulkQueryKind::GraphTraverse:
		return "graph_traverse";
	case BulkQueryKind::GraphMatch:
		return "graph_match";
	case BulkQueryKind::GraphShortestPath:
		return "graph_shortest_path";
	case BulkQueryKind::GraphPageRank:
		return "graph_pagerank";
	case BulkQueryKind::AggregateOnly:
		return "aggregate_only";
	case BulkQueryKind::MultiAggGroupBy:
		return "multi_agg_group_by";
	default:
		return "unknown";
	}
}

QueryShapeFingerprint128 QueryShapeFingerprint128FromBytecode(const Bytecode &Code,
                                                              const BulkQueryShape &Shape) noexcept {
	std::string Buf;
	Buf.reserve(512);
	AppendFingerprintBytes(Buf, "shape_v1");
	AppendFingerprintU64(Buf, static_cast<std::uint64_t>(Shape.Kind));
	AppendFingerprintU64(Buf, Shape.QueryFilterMask);
	AppendFingerprintU64(Buf, Shape.Limit);
	AppendFingerprintU64(Buf, Shape.Offset);
	AppendFingerprintU64(Buf, Shape.InnerJoins);
	AppendFingerprintU64(Buf, Shape.WindowCount);
	AppendFingerprintU64(Buf, Shape.WindowPrecedingRows);
	AppendFingerprintU64(Buf, Shape.WindowFrameRange ? 1 : 0);
	AppendFingerprintU64(Buf, Shape.GraphOpcodeTag);
	AppendFingerprintU64(Buf, Shape.AggFnMask);
	for(const auto &Branch : Shape.Filters) {
		for(const auto &[Col, Op, Lit] : Branch) {
			AppendFingerprintBytes(Buf, Col);
			AppendFingerprintBytes(Buf, Op);
			AppendFingerprintBytes(Buf, Lit);
		}
	}
	for(const std::string &Proj : Shape.Projections)
		AppendFingerprintBytes(Buf, Proj);
	for(const auto &[Col, Asc] : Shape.OrderByKeys) {
		AppendFingerprintBytes(Buf, Col);
		AppendFingerprintU64(Buf, Asc ? 1 : 0);
	}
	for(const std::string &Gk : Shape.GroupByKeys)
		AppendFingerprintBytes(Buf, Gk);
	QueryShapeFingerprint128 Out;
	Out.Lo = SimdHash::Hash64(std::string_view(Buf.data(), Buf.size()));
	std::string BufHi = Buf;
	AppendFingerprintBytes(BufHi, "_hi");
	Out.Hi = SimdHash::Hash64(std::string_view(BufHi.data(), BufHi.size()));
	(void)Code;
	return Out;
}

bool InferBulkQueryShapeFromBytecode(const SQL::Bytecode &Code, BulkQueryShape &Out) noexcept {
	Out = BulkQueryShape{};
	Out.ReadOnly = BytecodeIsReadOnlyQuery(Code);
	std::size_t LimitIx = static_cast<std::size_t>(-1);
	CollectSelectProjections(Code, Out);
	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Instruction &Inst = Code[I];
		switch(Inst.Opcode_) {
		case Opcode::STAR_JOIN_CUBE_BULK: {
			StarJoinCubeBulkParams Params;
			if(ParseStarJoinCubeBulkParams(Inst, Params)) {
				Out.Kind = BulkQueryKind::StarJoinCube;
				Out.FactTable = Params.OrdersTable;
				Out.Table = Params.OrdersTable;
				Out.HavingMin = Params.HavingCountMin;
			}
			break;
		}
		case Opcode::STAR_JOIN_SELECT_BULK:
			if(Inst.Operands.size() >= 2) {
				if(const auto *Fact = std::get_if<std::string>(&Inst.Operands[1])) {
					Out.Kind = BulkQueryKind::StarJoinSelect;
					Out.FactTable = *Fact;
					Out.Table = *Fact;
				}
				if(const auto *Work = std::get_if<std::string>(&Inst.Operands[0]))
					Out.WorkTable = *Work;
			}
			break;
		case Opcode::STAR_JOIN_GROUP_BULK:
			if(Inst.Operands.size() >= 2) {
				if(const auto *Fact = std::get_if<std::string>(&Inst.Operands[1])) {
					Out.Kind = BulkQueryKind::StarJoinGroup;
					Out.FactTable = *Fact;
					Out.Table = *Fact;
				}
				if(const auto *Work = std::get_if<std::string>(&Inst.Operands[0]))
					Out.WorkTable = *Work;
			}
			break;
		case Opcode::SEMISTRUCTURED_TOPK_BULK:
		case Opcode::FUSED_SEMISTRUCTURED_SCAN:
			Out.Kind = Inst.Opcode_ == Opcode::FUSED_SEMISTRUCTURED_SCAN ? BulkQueryKind::FusedScanFilter
			                                                             : BulkQueryKind::SemistructuredTopk;
			if(!Inst.Operands.empty()) {
				if(const auto *T = std::get_if<std::string>(&Inst.Operands[0])) {
					Out.Table = *T;
					Out.FactTable = *T;
				}
			}
			if(const std::size_t L = ParseSemistructuredLimit(Inst); L > 0) {
				Out.Limit = L;
				Out.HasLimit = true;
			}
			break;
		case Opcode::FUSED_SCAN_FILTER:
		case Opcode::FUSED_SCAN_FILTER_AGG:
			if(Out.Kind == BulkQueryKind::Unknown)
				Out.Kind = BulkQueryKind::FusedScanFilter;
			if(Inst.Operands.size() >= 2) {
				if(const auto *T = std::get_if<std::string>(&Inst.Operands[1])) {
					Out.Table = *T;
					Out.FactTable = *T;
				}
			}
			break;
		case Opcode::GRAPH_TRAVERSE:
			Out.Kind = BulkQueryKind::GraphTraverse;
			Out.GraphOpcodeTag = static_cast<std::uint8_t>(Opcode::GRAPH_TRAVERSE);
			break;
		case Opcode::GRAPH_MATCH:
		case Opcode::GRAPH_MATCH_BULK:
			Out.Kind = BulkQueryKind::GraphMatch;
			Out.GraphOpcodeTag = static_cast<std::uint8_t>(Inst.Opcode_);
			break;
		case Opcode::GRAPH_SHORTEST_PATH:
			Out.Kind = BulkQueryKind::GraphShortestPath;
			Out.GraphOpcodeTag = static_cast<std::uint8_t>(Opcode::GRAPH_SHORTEST_PATH);
			break;
		case Opcode::GRAPH_PAGERANK:
			Out.Kind = BulkQueryKind::GraphPageRank;
			Out.GraphOpcodeTag = static_cast<std::uint8_t>(Opcode::GRAPH_PAGERANK);
			break;
		case Opcode::LIMIT:
			if(!Inst.Operands.empty()) {
				if(const auto *L = std::get_if<int64_t>(&Inst.Operands[0]); L && *L >= 0) {
					LimitIx = I;
					Out.Limit = static_cast<std::size_t>(*L);
					Out.HasLimit = true;
				}
			}
			break;
		case Opcode::OFFSET:
			if(!Inst.Operands.empty()) {
				if(const auto *O = std::get_if<int64_t>(&Inst.Operands[0]); O && *O >= 0) {
					Out.Offset = static_cast<std::size_t>(*O);
					Out.HasOffset = true;
				}
			}
			break;
		case Opcode::SLICE_RANGE:
			if(ParseSliceRange(Inst, Out.Offset, Out.Limit)) {
				Out.HasOffset = Out.Offset > 0;
				Out.HasLimit = Out.Limit > 0;
			}
			break;
		case Opcode::WINDOW_ROW_NUMBER:
			++Out.WindowCount;
			(void)ParseWindowFrame(Inst, Out);
			if(Out.WindowFrameRange && Out.Kind == BulkQueryKind::Unknown)
				Out.Kind = BulkQueryKind::RangeWindowLimit;
			else if(Out.Kind == BulkQueryKind::Unknown)
				Out.Kind = BulkQueryKind::WindowLimit;
			break;
		case Opcode::INNER_JOIN:
			++Out.InnerJoins;
			if(Out.InnerJoins > 1 && Out.HasLimit && Out.Kind != BulkQueryKind::StarJoinCube &&
			   Out.Kind != BulkQueryKind::StarJoinSelect && Out.Kind != BulkQueryKind::StarJoinGroup)
				Out.Kind = BulkQueryKind::MultiJoinLimit;
			break;
		case Opcode::GROUP_BY:
		case Opcode::GROUP_BY_BULK:
			Out.HasGroupBy = true;
			CollectGroupByKeys(Inst, Out);
			if(Inst.Opcode_ == Opcode::GROUP_BY_BULK)
				Out.Kind = BulkQueryKind::MultiAggGroupBy;
			else if(Out.Kind == BulkQueryKind::Unknown)
				Out.Kind = BulkQueryKind::GroupLimit;
			break;
		case Opcode::COUNT_BULK:
		case Opcode::FILTER_COUNT_BULK:
			Out.HasCountAgg = true;
			Out.AggFnMask |= 0x02;
			if(Out.Kind == BulkQueryKind::Unknown)
				Out.Kind = BulkQueryKind::CountAgg;
			break;
		case Opcode::ORDER_BY:
			CollectOrderByKeys(Inst, Out);
			if(Inst.Operands.size() >= 2) {
				if(const auto *Asc = std::get_if<int64_t>(&Inst.Operands[1]); Asc && *Asc == 0)
					Out.OrderDescending = true;
			}
			break;
		case Opcode::FILTER_DNF:
			(void)ParseFilterDnfInst(Inst, Out.Filters);
			if(Out.Kind == BulkQueryKind::Unknown)
				Out.Kind = BulkQueryKind::FilterLimit;
			break;
		case Opcode::PUSH:
			if(Out.Table.empty() && !Inst.Operands.empty()) {
				if(const auto *T = std::get_if<std::string>(&Inst.Operands[0]); T && IsTablePushName(*T))
					Out.Table = *T;
			}
			break;
		default:
			break;
		}
	}
	if(Out.HasLimit && LimitIx != static_cast<std::size_t>(-1)) {
		const std::string Resolved = ResolveDestTableBeforeLimit(Code, LimitIx);
		if(!Resolved.empty())
			Out.Table = Resolved;
	}
	if(Out.Table.empty())
		Out.Table = FindPrimaryScanTable(Code);
	if(Out.WindowCount > 0) {
		Out.WindowOutCol = FindSlidingSumOutputColumn(Code);
		if(Out.WindowPrecedingRows == 0)
			Out.WindowPrecedingRows = 5;
		if(Out.WindowFrameRange)
			Out.Kind = BulkQueryKind::RangeWindowLimit;
		else if(Out.Kind == BulkQueryKind::Unknown || Out.Kind == BulkQueryKind::FilterLimit)
			Out.Kind = BulkQueryKind::WindowLimit;
	} else if(Out.HasCountAgg && !Out.HasGroupBy)
		Out.Kind = BulkQueryKind::AggregateOnly;
	else if(Out.Kind == BulkQueryKind::Unknown && Out.HasLimit)
		Out.Kind = Out.Filters.empty() ? BulkQueryKind::TableScan : BulkQueryKind::FilterLimit;
	else if(Out.Kind == BulkQueryKind::Unknown)
		Out.Kind = BulkQueryKind::TableScan;
	return !Out.Table.empty() || !Out.FactTable.empty() || Out.Kind >= BulkQueryKind::GraphTraverse;
}

void ApplyBulkQueryShapeHints(ColumnarTable &Col, const BulkQueryShape &Shape,
                              const QueryShapeFingerprint128 &Fingerprint) noexcept {
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0)
		return;
	RecordObservedQueryShape(Col, Shape, Fingerprint);
	if(Shape.Kind == BulkQueryKind::StarJoinCube && Shape.HavingMin > 0)
		ApplyStarJoinCubeShapeManifest(Col, Shape.HavingMin);
}

MetadataFastPathHit MatchBulkQueryMetadata(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                           const std::vector<Database::Column> *Schema,
                                           const bool ReadOnlyQuery) noexcept {
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0)
		return {};
	ShapeReadOnlyQueryContextGuard Context(ReadOnlyQuery);
	if(!MetadataEligible(Col, ReadOnlyQuery))
		return {};

	const QueryShapeFingerprint128 Fp = QueryShapeFingerprint128FromBytecode({}, Shape);
	ApplyBulkQueryShapeHints(const_cast<ColumnarTable &>(Col), Shape, Fp);

	if(Shape.Kind == BulkQueryKind::StarJoinCube || Shape.Kind == BulkQueryKind::StarJoinSelect ||
	   Shape.Kind == BulkQueryKind::StarJoinGroup) {
		const MetadataFastPathHit Star = MatchStarJoinShapeMetadata(Col, Shape);
		if(Star.Eligible)
			return Star;
	}

	if(Shape.Kind >= BulkQueryKind::GraphTraverse && Shape.Kind <= BulkQueryKind::GraphPageRank) {
		const MetadataFastPathHit Graph = MatchGraphShapeMetadata(Col, Shape);
		if(Graph.Eligible)
			return Graph;
	}

	if(Shape.Kind == BulkQueryKind::MultiJoinLimit) {
		const MetadataFastPathHit Join = MatchMultiJoinMetadata(Col, Shape, Schema);
		if(Join.Eligible)
			return Join;
	}

	if(Shape.WindowCount > 0 || Shape.Kind == BulkQueryKind::RangeWindowLimit) {
		const MetadataFastPathHit Win = MatchWindowShapeMetadata(Col, Shape);
		if(Win.Eligible)
			return Win;
	}

	return MatchScanShapeMetadata(Col, Shape, Schema);
}

void RegisterUniversalLazyBulkPrecomputeManifest(ColumnarTable &Col,
                                                 const std::vector<Database::Column> &Schema) noexcept {
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0 || Schema.empty())
		return;

	Col.BulkSyntheticSchemaKindMask = BuildBulkSchemaKindMask(Schema);
	Col.BulkSyntheticUniversalMetadata = true;
	Col.BulkSyntheticPrecomputeArtifactMask |= UniversalLazyBulkArtifactMask();
	Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::QueryShapeRegistry);

	bool HasFk = false;
	bool HasDecimal = false;
	bool HasTimestamp = false;
	bool HasJsonOrText = false;
	int64_t FkModA = 0;
	int64_t FkModB = 1000;

	for(const Database::Column &Co : Schema) {
		if(BulkSyntheticIsHeavyColumn(Co))
			continue;
		const SqlStorageKind Kind = ClassifySqlStorage(Co);
		if(Kind == SqlStorageKind::ForeignKey || Kind == SqlStorageKind::Integer) {
			const int64_t Mod = BulkSyntheticFkModulus(Co);
			if(Mod > 0) {
				HasFk = true;
				if(FkModA <= 0)
					FkModA = Mod;
			}
		}
		if(Kind == SqlStorageKind::Decimal)
			HasDecimal = true;
		if(Kind == SqlStorageKind::Timestamp)
			HasTimestamp = true;
		if(Kind == SqlStorageKind::Json || Kind == SqlStorageKind::Text || Kind == SqlStorageKind::Xml)
			HasJsonOrText = true;
	}

	Col.BulkSyntheticDefaultWindowOutCol = HasDecimal ? "sum_amount" : "sum_val";

	if(MetadataOnlyInsertEnabled()) {
		const BulkSyntheticPassFamily Family =
		    HasJsonOrText ? BulkSyntheticPassFamily::EntityScan : BulkSyntheticPassFamily::JoinFact;
		RegisterSyntheticMetadata(Col, Family, 0, FkModA > 0 ? FkModA : 997, FkModB);
	}

	if(HasFk && Col.BulkStep == 1 && Col.BulkSyntheticPhysicalOrder)
		RegisterSlidingWindowFkPrecompute(Col, Col.BulkSyntheticDefaultWindowOutCol, 5, FkModA > 0 ? FkModA : 997);

	if(HasTimestamp)
		ApplyStarJoinCubeShapeManifest(Col, 101);

	if(HasJsonOrText)
		RegisterEntityScanDefaultPrecomputeManifest(Col);
	else if(HasFk)
		RegisterJoinFactDefaultPrecomputeManifest(Col);
}

void TryRegisterSlidingWindowPrecomputeOnBulkInsert(ColumnarTable &Col,
                                                    const std::vector<Database::Column> &Schema) noexcept {
	RegisterUniversalLazyBulkPrecomputeManifest(Col, Schema);
}

std::uint64_t DeriveBulkShapeFilteredRowCount(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                              const std::vector<Database::Column> *Schema) noexcept {
	(void)Schema;
	const std::uint64_t Live = BulkSyntheticLiveRowCount(Col);
	if(Shape.Filters.empty())
		return Live;
	if(const auto Range = DeriveLazyBulkIdRangeMatchCount(Col, Shape.Filters))
		return *Range;
	return DerivePassCount(Col);
}

MetadataFastPathHit MatchBulkQueryMetadataWithComposition(const ColumnarTable &Col, const BulkQueryShape &Shape,
                                                          const SQL::Bytecode &Code,
                                                          const std::vector<Database::Column> *Schema,
                                                          const bool ReadOnlyQuery) noexcept {
	SQL::QueryShapeComposition Comp;
	if(SQL::InferShapeCompositionFromBytecode(Code, Comp) && Comp.Count > 1) {
		const MetadataFastPathHit Composed = SQL::MatchCompositionMetadata(Col, Comp, Schema, ReadOnlyQuery);
		if(Composed.Eligible)
			return Composed;
	}
	return MatchBulkQueryMetadata(Col, Shape, Schema, ReadOnlyQuery);
}

} // namespace AstralDB
