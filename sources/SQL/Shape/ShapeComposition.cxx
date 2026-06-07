#include <SQL/Shape/ShapeComposition.hxx>
#include <SQL/Shape/ShapeCompositionTypes.hxx>

#include <Database/Storage/BulkQueryMetadata.hxx>
#include <Database/Storage/BulkSyntheticDerive.hxx>
#include <SQL/Fusion/FusionPass.hxx>
#include <SQL/SQL.hxx>

#include <DS/SimdHash.hxx>

#include <algorithm>
#include <functional>
#include <limits>

namespace AstralDB {
namespace SQL {
namespace {

using SQL::Instruction;
using SQL::Opcode;

void AppendFingerprintU64(std::string &Buf, const std::uint64_t V) noexcept {
	for(int I = 0; I < 8; ++I)
		Buf.push_back(static_cast<char>((V >> (I * 8)) & 0xFF));
}

[[nodiscard]] std::uint16_t AddNode(QueryShapeComposition &Out, ShapeCompositionNode Node) noexcept {
	if(Out.Count >= QueryShapeComposition::MaxNodes)
		return UINT16_MAX;
	const std::uint16_t Idx = static_cast<std::uint16_t>(Out.Count);
	Out.Nodes[Out.Count++] = std::move(Node);
	return Idx;
}

[[nodiscard]] ElementaryShapeKind SetOpKindFromCompound(const CompoundSetOpKind Op) noexcept {
	switch(Op) {
	case CompoundSetOpKind::UnionAll:
		return ElementaryShapeKind::UnionAll;
	case CompoundSetOpKind::UnionDistinct:
		return ElementaryShapeKind::UnionDistinct;
	case CompoundSetOpKind::Intersect:
	case CompoundSetOpKind::IntersectAll:
		return ElementaryShapeKind::Intersect;
	case CompoundSetOpKind::Except:
	case CompoundSetOpKind::ExceptAll:
		return ElementaryShapeKind::Except;
	}
	return ElementaryShapeKind::None;
}

[[nodiscard]] ElementaryShapeKind SetOpKindFromOperand(const int64_t Mode) noexcept {
	return SetOpKindFromCompound(static_cast<CompoundSetOpKind>(Mode));
}

void CopySelectPayload(const SelectAST &Sel, BulkQueryShape &Payload) noexcept {
	Payload.Table = Sel.SourceTableName();
	Payload.FactTable = Sel.SourceTableName();
	if(Sel.SelectLimitValue() >= 0) {
		Payload.Limit = static_cast<std::size_t>(Sel.SelectLimitValue());
		Payload.HasLimit = true;
	}
	if(Sel.SelectOffsetValue() > 0) {
		Payload.Offset = static_cast<std::size_t>(Sel.SelectOffsetValue());
		Payload.HasOffset = true;
	}
	Payload.HasGroupBy = !Sel.GroupKeys().empty();
	Payload.GroupByKeys = Sel.GroupKeys();
	Payload.HasCountAgg = Sel.AggKind() != GroupAggMode::None || !Sel.ComboAggs().empty();
	Payload.ReadOnly = true;
	for(const auto &Spec : Sel.OrderBySpecs())
		Payload.OrderByKeys.emplace_back(Spec.Column, Spec.Ascending);
	for(const auto &Col : Sel.ProjectionColumns())
		Payload.Projections.push_back(Col);
	Payload.Filters.clear();
	if(Sel.WhereRoot()) {
		std::vector<FilterTriple> Simple;
		if(FusionPass::ExtractSimplePredicates(Sel.WhereRoot(), Simple)) {
			std::vector<FilterPredicateTriple> Branch;
			for(const auto &F : Simple)
				Branch.emplace_back(F.Column, F.Op, F.Literal);
			if(!Branch.empty())
				Payload.Filters.push_back(std::move(Branch));
		}
	}
	Payload.InnerJoins = Sel.JoinSpecs().size();
}

[[nodiscard]] std::uint16_t WrapUnary(QueryShapeComposition &Out, const std::uint16_t Child,
                                      const ElementaryShapeKind Kind, BulkQueryShape Payload) noexcept {
	if(Child == UINT16_MAX)
		return UINT16_MAX;
	ShapeCompositionNode Node;
	Node.Kind = Kind;
	Node.ChildA = Child;
	Node.Payload = std::move(Payload);
	return AddNode(Out, std::move(Node));
}

[[nodiscard]] std::uint16_t BuildSelectPipeline(const SelectAST &Sel, QueryShapeComposition &Out) noexcept {
	BulkQueryShape BasePayload;
	CopySelectPayload(Sel, BasePayload);
	if(Sel.HasFusedPlan()) {
		const FusionPlan &Fp = Sel.FusedPlan();
		if(!Fp.Table.empty())
			BasePayload.Table = BasePayload.FactTable = Fp.Table;
		if(Fp.Limit >= 0) {
			BasePayload.Limit = static_cast<std::size_t>(Fp.Limit);
			BasePayload.HasLimit = true;
		}
		if(Fp.Offset > 0) {
			BasePayload.Offset = static_cast<std::size_t>(Fp.Offset);
			BasePayload.HasOffset = true;
		}
	}

	ShapeCompositionNode ScanNode;
	if(Sel.HasFusedPlan() && Sel.FusedPlan().Kind == FusionKind::StarJoinCube)
		ScanNode.Kind = ElementaryShapeKind::StarJoin;
	else if(Sel.HasFusedPlan() &&
	        (Sel.FusedPlan().Kind == FusionKind::ScanFilter || Sel.FusedPlan().Kind == FusionKind::ScanFilterProjectLimit))
		ScanNode.Kind = ElementaryShapeKind::FusedScanFilter;
	else
		ScanNode.Kind = ElementaryShapeKind::Scan;
	ScanNode.Payload = BasePayload;
	std::uint16_t Cur = AddNode(Out, std::move(ScanNode));
	if(Cur == UINT16_MAX)
		return UINT16_MAX;

	if(Sel.WhereRoot() && !Sel.HasFusedPlan()) {
		BulkQueryShape FilterPayload = BasePayload;
		FilterPayload.Kind = BulkQueryKind::FilterLimit;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Filter, std::move(FilterPayload));
	}
	for(std::size_t Ji = 0; Ji < Sel.JoinSpecs().size(); ++Ji) {
		BulkQueryShape JoinPayload = BasePayload;
		JoinPayload.InnerJoins = Ji + 1;
		JoinPayload.Kind = JoinPayload.InnerJoins > 1 ? BulkQueryKind::MultiJoinLimit : BulkQueryKind::FilterLimit;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::InnerJoin, std::move(JoinPayload));
	}
	if(!Sel.GroupKeys().empty()) {
		BulkQueryShape GbPayload = BasePayload;
		GbPayload.Kind = BulkQueryKind::GroupLimit;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::GroupBy, std::move(GbPayload));
	}
	if(Sel.HavingRoot()) {
		BulkQueryShape HavingPayload = BasePayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Having, std::move(HavingPayload));
	}
	if(!Sel.WindowSpecs().empty()) {
		BulkQueryShape WinPayload = BasePayload;
		WinPayload.WindowCount = Sel.WindowSpecs().size();
		WinPayload.Kind = BulkQueryKind::WindowLimit;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Window, std::move(WinPayload));
	}
	if(Sel.DistinctSelected()) {
		BulkQueryShape DistPayload = BasePayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Distinct, std::move(DistPayload));
	}
	if(!Sel.OrderBySpecs().empty()) {
		BulkQueryShape OrdPayload = BasePayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::OrderBy, std::move(OrdPayload));
	}
	if(Sel.SelectOffsetValue() > 0) {
		BulkQueryShape OffPayload = BasePayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Offset, std::move(OffPayload));
	}
	if(Sel.SelectLimitValue() >= 0) {
		BulkQueryShape LimPayload = BasePayload;
		LimPayload.Kind = BulkQueryKind::FilterLimit;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Limit, std::move(LimPayload));
	}
	if(Sel.AggKind() != GroupAggMode::None && Sel.GroupKeys().empty()) {
		BulkQueryShape AggPayload = BasePayload;
		AggPayload.Kind = BulkQueryKind::AggregateOnly;
		AggPayload.HasCountAgg = true;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::AggregateScalar, std::move(AggPayload));
	}
	return Cur;
}

[[nodiscard]] std::size_t EstimateNodeRows(const ColumnarTable &Col, const QueryShapeComposition &Comp,
                                           const std::uint16_t Idx, const std::vector<Database::Column> *Schema,
                                           std::array<std::size_t, QueryShapeComposition::MaxNodes> &Memo) noexcept {
	if(Idx >= Comp.Count)
		return 0;
	if(Memo[Idx] != std::numeric_limits<std::size_t>::max())
		return Memo[Idx];
	const ShapeCompositionNode &Node = Comp.Nodes[Idx];
	const auto EstimateChild = [&](const std::uint16_t C) {
		return C == UINT16_MAX ? Col.RowCount : EstimateNodeRows(Col, Comp, C, Schema, Memo);
	};
	std::size_t Rows = Col.RowCount;
	switch(Node.Kind) {
	case ElementaryShapeKind::Scan:
	case ElementaryShapeKind::FusedScanFilter:
	case ElementaryShapeKind::StarJoin:
	case ElementaryShapeKind::GraphOp:
		Rows = static_cast<std::size_t>(
		    std::min<std::uint64_t>(Col.RowCount, DeriveBulkShapeFilteredRowCount(Col, Node.Payload, Schema)));
		break;
	case ElementaryShapeKind::Filter:
	case ElementaryShapeKind::Having:
	case ElementaryShapeKind::Project:
	case ElementaryShapeKind::Distinct:
	case ElementaryShapeKind::OrderBy:
	case ElementaryShapeKind::CteScan:
	case ElementaryShapeKind::SubqueryScan:
		Rows = static_cast<std::size_t>(std::min<std::uint64_t>(
		    EstimateChild(Node.ChildA), DeriveBulkShapeFilteredRowCount(Col, Node.Payload, Schema)));
		break;
	case ElementaryShapeKind::InnerJoin: {
		const std::size_t Left = EstimateChild(Node.ChildA);
		const std::uint64_t JoinShrink = static_cast<std::uint64_t>(1 + Node.Payload.InnerJoins);
		Rows = static_cast<std::size_t>(std::max<std::uint64_t>(1, Left / JoinShrink));
		break;
	}
	case ElementaryShapeKind::GroupBy:
	case ElementaryShapeKind::AggregateScalar: {
		const std::size_t Passing = EstimateChild(Node.ChildA);
		std::uint64_t Den = 100;
		if(Col.BulkSyntheticUniversalMetadata) {
			const int64_t ModA = Col.BulkPartitionMod > 0 ? Col.BulkPartitionMod : Col.BulkSyntheticCachedFkModA;
			const int64_t ModB = Col.BulkSyntheticCachedFkModB > 0 ? Col.BulkSyntheticCachedFkModB : 1000;
			Den = static_cast<std::uint64_t>(std::max<int64_t>(ModA, ModB));
			if(Den < 2)
				Den = 100;
		}
		Rows = static_cast<std::size_t>(std::max<std::uint64_t>(1, Passing / Den));
		if(Node.Payload.HasLimit && Node.Payload.Limit > 0)
			Rows = std::min(Rows, Node.Payload.Limit);
		break;
	}
	case ElementaryShapeKind::Window:
		Rows = EstimateChild(Node.ChildA);
		if(Node.Payload.HasLimit && Node.Payload.Limit > 0)
			Rows = std::min(Rows, Node.Payload.Limit);
		break;
	case ElementaryShapeKind::Limit:
	case ElementaryShapeKind::Offset: {
		Rows = EstimateChild(Node.ChildA);
		if(Node.Kind == ElementaryShapeKind::Limit && Node.Payload.HasLimit)
			Rows = std::min(Rows, Node.Payload.Limit);
		if(Node.Kind == ElementaryShapeKind::Offset && Node.Payload.HasOffset && Node.Payload.Offset < Rows)
			Rows -= Node.Payload.Offset;
		break;
	}
	case ElementaryShapeKind::UnionAll:
		Rows = EstimateChild(Node.ChildA) + EstimateChild(Node.ChildB);
		break;
	case ElementaryShapeKind::UnionDistinct: {
		const std::size_t A = EstimateChild(Node.ChildA);
		const std::size_t B = EstimateChild(Node.ChildB);
		Rows = std::max(A, B) + std::min(A, B) / 2;
		break;
	}
	case ElementaryShapeKind::Intersect:
		Rows = std::min(EstimateChild(Node.ChildA), EstimateChild(Node.ChildB));
		break;
	case ElementaryShapeKind::Except: {
		const std::size_t A = EstimateChild(Node.ChildA);
		const std::size_t B = EstimateChild(Node.ChildB);
		Rows = A > B ? A - B : 0;
		break;
	}
	case ElementaryShapeKind::ExistsSemi:
		Rows = EstimateChild(Node.ChildA) / 10 + 1;
		break;
	default:
		Rows = EstimateChild(Node.ChildA);
		break;
	}
	if(Node.Payload.HasLimit && Node.Payload.Limit > 0 && Node.Kind != ElementaryShapeKind::Limit)
		Rows = std::min(Rows, Node.Payload.Limit);
	Memo[Idx] = Rows;
	return Rows;
}

[[nodiscard]] std::uint16_t InferLinearPipeline(const Bytecode &Code, QueryShapeComposition &Out,
                                                BulkQueryShape &LeafPayload) noexcept {
	if(!InferBulkQueryShapeFromBytecode(Code, LeafPayload))
		return UINT16_MAX;

	ShapeCompositionNode Root;
	switch(LeafPayload.Kind) {
	case BulkQueryKind::StarJoinCube:
	case BulkQueryKind::StarJoinSelect:
	case BulkQueryKind::StarJoinGroup:
		Root.Kind = ElementaryShapeKind::StarJoin;
		break;
	case BulkQueryKind::FusedScanFilter:
	case BulkQueryKind::SemistructuredTopk:
		Root.Kind = ElementaryShapeKind::FusedScanFilter;
		break;
	case BulkQueryKind::GraphTraverse:
	case BulkQueryKind::GraphMatch:
	case BulkQueryKind::GraphShortestPath:
	case BulkQueryKind::GraphPageRank:
		Root.Kind = ElementaryShapeKind::GraphOp;
		break;
	default:
		Root.Kind = ElementaryShapeKind::Scan;
		break;
	}
	Root.Payload = LeafPayload;
	std::uint16_t Cur = AddNode(Out, std::move(Root));
	if(Cur == UINT16_MAX)
		return UINT16_MAX;

	if(!LeafPayload.Filters.empty() && Root.Kind == ElementaryShapeKind::Scan) {
		BulkQueryShape FilterPayload = LeafPayload;
		FilterPayload.Kind = BulkQueryKind::FilterLimit;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Filter, std::move(FilterPayload));
	}
	if(LeafPayload.InnerJoins > 0) {
		BulkQueryShape JoinPayload = LeafPayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::InnerJoin, std::move(JoinPayload));
	}
	if(LeafPayload.HasGroupBy) {
		BulkQueryShape GbPayload = LeafPayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::GroupBy, std::move(GbPayload));
	}
	if(LeafPayload.WindowCount > 0) {
		BulkQueryShape WinPayload = LeafPayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Window, std::move(WinPayload));
	}
	if(!LeafPayload.OrderByKeys.empty()) {
		BulkQueryShape OrdPayload = LeafPayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::OrderBy, std::move(OrdPayload));
	}
	if(LeafPayload.HasOffset) {
		BulkQueryShape OffPayload = LeafPayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Offset, std::move(OffPayload));
	}
	if(LeafPayload.HasLimit) {
		BulkQueryShape LimPayload = LeafPayload;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Limit, std::move(LimPayload));
	}
	if(LeafPayload.HasCountAgg && !LeafPayload.HasGroupBy) {
		BulkQueryShape AggPayload = LeafPayload;
		AggPayload.Kind = BulkQueryKind::AggregateOnly;
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::AggregateScalar, std::move(AggPayload));
	}
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::FUSED_SEMI_JOIN_EXISTS || Inst.Opcode_ == Opcode::SEMI_JOIN_HASH ||
		   Inst.Opcode_ == Opcode::EXISTS) {
			BulkQueryShape SemiPayload = LeafPayload;
			Cur = WrapUnary(Out, Cur, ElementaryShapeKind::ExistsSemi, std::move(SemiPayload));
			break;
		}
	}
	return Cur;
}

} // namespace

bool BuildShapeCompositionFromSelect(const SelectAST &Sel, QueryShapeComposition &Out) noexcept {
	Out = {};
	const std::uint16_t Root = BuildSelectPipeline(Sel, Out);
	if(Root == UINT16_MAX || Out.Count == 0)
		return false;
	Out.Root = Root;
	return true;
}

bool BuildShapeCompositionFromCompound(const CompoundSelectAST &Comp, QueryShapeComposition &Out) noexcept {
	Out = {};
	if(Comp.Arms.empty())
		return false;
	std::uint16_t Acc = UINT16_MAX;
	for(std::size_t I = 0; I < Comp.Arms.size(); ++I) {
		if(!Comp.Arms[I])
			return false;
		const std::uint16_t ArmRoot = BuildSelectPipeline(*Comp.Arms[I], Out);
		if(ArmRoot == UINT16_MAX)
			return false;
		if(Acc == UINT16_MAX) {
			Acc = ArmRoot;
			continue;
		}
		ShapeCompositionNode SetNode;
		SetNode.Kind = SetOpKindFromCompound(Comp.Ops[I - 1]);
		SetNode.ChildA = Acc;
		SetNode.ChildB = ArmRoot;
		SetNode.Payload.ReadOnly = true;
		SetNode.Payload.HasLimit = Comp.Limit >= 0;
		if(Comp.Limit >= 0)
			SetNode.Payload.Limit = static_cast<std::size_t>(Comp.Limit);
		Acc = AddNode(Out, std::move(SetNode));
		if(Acc == UINT16_MAX)
			return false;
	}
	if(Acc == UINT16_MAX)
		return false;
	std::uint16_t Cur = Acc;
	BulkQueryShape TailPayload;
	TailPayload.ReadOnly = true;
	if(Comp.Limit >= 0) {
		TailPayload.HasLimit = true;
		TailPayload.Limit = static_cast<std::size_t>(Comp.Limit);
	}
	if(Comp.Offset > 0) {
		TailPayload.HasOffset = true;
		TailPayload.Offset = static_cast<std::size_t>(Comp.Offset);
	}
	if(!Comp.OrderByColumns.empty()) {
		for(const auto &Spec : Comp.OrderByColumns)
			TailPayload.OrderByKeys.emplace_back(Spec.Column, Spec.Ascending);
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::OrderBy, TailPayload);
	}
	if(Comp.Offset > 0)
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Offset, TailPayload);
	if(Comp.Limit >= 0)
		Cur = WrapUnary(Out, Cur, ElementaryShapeKind::Limit, TailPayload);
	Out.Root = Cur;
	return Out.Count > 0 && Out.Root != UINT16_MAX;
}

bool InferShapeCompositionFromBytecode(const Bytecode &Code, QueryShapeComposition &Out) noexcept {
	Out = {};
	if(Code.empty())
		return false;

	std::vector<std::uint16_t> SetCombineRoots;
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ != Opcode::SET_COMBINE || Inst.Operands.size() < 4)
			continue;
		const auto *Mode = std::get_if<int64_t>(&Inst.Operands[3]);
		if(!Mode)
			continue;
		ShapeCompositionNode SetNode;
		SetNode.Kind = SetOpKindFromOperand(*Mode);
		SetNode.Payload.ReadOnly = true;
		BulkQueryShape LeftPayload;
		BulkQueryShape RightPayload;
		(void)InferBulkQueryShapeFromBytecode(Code, LeftPayload);
		(void)InferBulkQueryShapeFromBytecode(Code, RightPayload);
		SetNode.Payload = LeftPayload;
		const std::uint16_t Left = InferLinearPipeline(Code, Out, LeftPayload);
		const std::uint16_t Right = InferLinearPipeline(Code, Out, RightPayload);
		if(Left == UINT16_MAX || Right == UINT16_MAX)
			continue;
		SetNode.ChildA = Left;
		SetNode.ChildB = Right;
		SetCombineRoots.push_back(AddNode(Out, std::move(SetNode)));
	}

	if(!SetCombineRoots.empty()) {
		Out.Root = SetCombineRoots.back();
		return true;
	}

	BulkQueryShape LeafPayload;
	const std::uint16_t Root = InferLinearPipeline(Code, Out, LeafPayload);
	if(Root == UINT16_MAX || Out.Count == 0)
		return false;
	Out.Root = Root;
	return true;
}

QueryShapeFingerprint128 QueryShapeFingerprint128FromComposition(const QueryShapeComposition &Comp) noexcept {
	std::string Buf;
	Buf.reserve(Comp.Count * 48 + 16);
	AppendFingerprintU64(Buf, static_cast<std::uint64_t>(Comp.Count));
	AppendFingerprintU64(Buf, static_cast<std::uint64_t>(Comp.Root));
	for(std::size_t I = 0; I < Comp.Count; ++I) {
		const ShapeCompositionNode &N = Comp.Nodes[I];
		Buf.push_back(static_cast<char>(N.Kind));
		AppendFingerprintU64(Buf, static_cast<std::uint64_t>(N.ChildA));
		AppendFingerprintU64(Buf, static_cast<std::uint64_t>(N.ChildB));
		const QueryShapeFingerprint128 LeafFp = QueryShapeFingerprint128FromBytecode({}, N.Payload);
		AppendFingerprintU64(Buf, LeafFp.Lo);
		AppendFingerprintU64(Buf, LeafFp.Hi);
	}
	QueryShapeFingerprint128 Out;
	std::string BufHi = Buf;
	BufHi.push_back('\1');
	Out.Lo = SimdHash::Hash64(std::string_view(Buf.data(), Buf.size()));
	Out.Hi = SimdHash::Hash64(std::string_view(BufHi.data(), BufHi.size()));
	return Out;
}

MetadataFastPathHit MatchCompositionMetadata(const ColumnarTable &Col, const QueryShapeComposition &Comp,
                                             const std::vector<Database::Column> *Schema,
                                             const bool ReadOnlyQuery) noexcept {
	MetadataFastPathHit Out;
	if(Comp.Root == UINT16_MAX || Comp.Count == 0 || !Col.BulkSyntheticLazy || Col.RowCount == 0)
		return Out;
	if(!MetadataTierAllowsFastPath(ClassifyMetadataEligibility(Col, ReadOnlyQuery)))
		return Out;

	const ShapeCompositionNode &Root = Comp.Nodes[Comp.Root];
	const MetadataFastPathHit LeafHit = MatchBulkQueryMetadata(Col, Root.Payload, Schema, ReadOnlyQuery);
	if(LeafHit.Eligible && Comp.Count == 1)
		return LeafHit;

	std::array<std::size_t, QueryShapeComposition::MaxNodes> Memo;
	Memo.fill(std::numeric_limits<std::size_t>::max());
	const std::size_t Estimated = EstimateNodeRows(Col, Comp, Comp.Root, Schema, Memo);
	if(Estimated == 0 && !Root.Payload.HasLimit)
		return Out;

	Out.Eligible = true;
	Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
	Out.ResultRows = Estimated;
	if(LeafHit.Eligible)
		Out.ResultRows = std::min(Out.ResultRows, LeafHit.ResultRows > 0 ? LeafHit.ResultRows : Estimated);
	if(Root.Payload.HasLimit && Root.Payload.Limit > 0)
		Out.ResultRows = std::min(Out.ResultRows, Root.Payload.Limit);
	return Out;
}

const char *ElementaryShapeKindName(const ElementaryShapeKind Kind) noexcept {
	switch(Kind) {
	case ElementaryShapeKind::Scan:
		return "scan";
	case ElementaryShapeKind::Filter:
		return "filter";
	case ElementaryShapeKind::Project:
		return "project";
	case ElementaryShapeKind::InnerJoin:
		return "inner_join";
	case ElementaryShapeKind::GroupBy:
		return "group_by";
	case ElementaryShapeKind::Having:
		return "having";
	case ElementaryShapeKind::OrderBy:
		return "order_by";
	case ElementaryShapeKind::Limit:
		return "limit";
	case ElementaryShapeKind::Offset:
		return "offset";
	case ElementaryShapeKind::Window:
		return "window";
	case ElementaryShapeKind::Distinct:
		return "distinct";
	case ElementaryShapeKind::UnionAll:
		return "union_all";
	case ElementaryShapeKind::UnionDistinct:
		return "union_distinct";
	case ElementaryShapeKind::Intersect:
		return "intersect";
	case ElementaryShapeKind::Except:
		return "except";
	case ElementaryShapeKind::SubqueryScan:
		return "subquery";
	case ElementaryShapeKind::CteScan:
		return "cte";
	case ElementaryShapeKind::AggregateScalar:
		return "agg_scalar";
	case ElementaryShapeKind::ExistsSemi:
		return "exists_semi";
	case ElementaryShapeKind::FusedScanFilter:
		return "fused_scan_filter";
	case ElementaryShapeKind::StarJoin:
		return "star_join";
	case ElementaryShapeKind::GraphOp:
		return "graph";
	default:
		return "none";
	}
}

BulkQueryKind BulkQueryKindFromCompositionRoot(const QueryShapeComposition &Comp) noexcept {
	if(Comp.Root == UINT16_MAX || Comp.Count == 0)
		return BulkQueryKind::Unknown;
	const ShapeCompositionNode &Root = Comp.Nodes[Comp.Root];
	if(Root.Kind == ElementaryShapeKind::StarJoin || Root.Kind == ElementaryShapeKind::GraphOp)
		return Root.Payload.Kind;
	return Root.Payload.Kind != BulkQueryKind::Unknown ? Root.Payload.Kind : BulkQueryKind::TableScan;
}

bool ShapeCompositionPass::Run(Tree<ASTNode> &Ast) noexcept {
	bool Any = false;
	for(auto &Root : Ast.Nodes_) {
		if(!Root)
			continue;
		std::function<void(Tree<ASTNode>::Node *)> Walk = [&](Tree<ASTNode>::Node *Node) {
			if(!Node)
				return;
			for(auto &Child : Node->Children)
				Walk(Child.get());
			if(auto *Sel = dynamic_cast<SelectAST *>(Node->Value.get())) {
				QueryShapeComposition Comp;
				if(BuildShapeCompositionFromSelect(*Sel, Comp)) {
					Sel->SetShapeComposition(std::move(Comp));
					Any = true;
				}
			} else if(auto *CompAst = dynamic_cast<CompoundSelectAST *>(Node->Value.get())) {
				QueryShapeComposition Comp;
				if(BuildShapeCompositionFromCompound(*CompAst, Comp)) {
					CompAst->SetShapeComposition(std::move(Comp));
					Any = true;
				}
			}
		};
		Walk(Root.get());
	}
	return Any;
}

} // namespace SQL
} // namespace AstralDB
