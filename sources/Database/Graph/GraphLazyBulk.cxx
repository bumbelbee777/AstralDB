#include <Database/Graph/GraphLazyBulk.hxx>

#include <Database/Database.hxx>
#include <IO/Limits.hxx>

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace AstralDB {
namespace {

enum class VertexKind : std::uint8_t { Customer = 0, Product = 1 };

struct BfsState {
	std::int64_t Id = 0;
	VertexKind Kind = VertexKind::Customer;
	int Depth = 0;
};

[[nodiscard]] bool LabelPasses(const GraphSpec &Spec, std::string_view Filter) noexcept {
	if(Filter.empty())
		return true;
	if(Spec.EdgeLabelCol.empty())
		return Filter == "ordered";
	return false;
}

[[nodiscard]] std::int64_t ParseAnchorId(std::string_view Raw) {
	try {
		return std::stoll(std::string(Raw));
	} catch(...) {
		return 1;
	}
}

void ExpandCustomer(const ColumnarTable &Orders, std::int64_t CustId, std::unordered_set<std::int64_t> &Prods) {
	const int64_t CustMod = Orders.BulkSyntheticFkCustMod;
	const int64_t ProdMod = Orders.BulkSyntheticFkProdMod;
	if(CustMod <= 0 || ProdMod <= 0 || CustId <= 0)
		return;
	const std::int64_t RowCount = static_cast<std::int64_t>(Orders.RowCount);
	for(std::int64_t RowId = CustId; RowId <= RowCount; RowId += CustMod) {
		const std::int64_t Prod = (RowId - 1) % ProdMod + 1;
		Prods.insert(Prod);
	}
}

void ExpandProduct(const ColumnarTable &Orders, std::int64_t ProdId, std::unordered_set<std::int64_t> &Custs) {
	const int64_t CustMod = Orders.BulkSyntheticFkCustMod;
	const int64_t ProdMod = Orders.BulkSyntheticFkProdMod;
	if(CustMod <= 0 || ProdMod <= 0 || ProdId <= 0)
		return;
	const std::int64_t RowCount = static_cast<std::int64_t>(Orders.RowCount);
	for(std::int64_t RowId = ProdId; RowId <= RowCount; RowId += ProdMod) {
		const std::int64_t Cust = (RowId - 1) % CustMod + 1;
		Custs.insert(Cust);
	}
}

std::unordered_map<std::int64_t, int> RunSegmentBfs(const ColumnarTable &Orders, const GraphMatchBulkSegment &Seg,
                                                    VertexKind StartKind,
                                                    const std::unordered_set<std::int64_t> &StartIds,
                                                    VertexKind EndKind) {
	std::unordered_map<std::int64_t, int> Best;
	std::deque<BfsState> Q;
	std::unordered_set<std::uint64_t> Seen;
	for(const std::int64_t Id : StartIds) {
		Q.push_back({Id, StartKind, 0});
		Seen.insert((static_cast<std::uint64_t>(StartKind) << 56) | static_cast<std::uint64_t>(Id));
	}
	while(!Q.empty()) {
		const BfsState St = Q.front();
		Q.pop_front();
		if(St.Depth >= Seg.MaxHops)
			continue;
		std::unordered_set<std::int64_t> Neighbors;
		if(St.Kind == VertexKind::Customer)
			ExpandCustomer(Orders, St.Id, Neighbors);
		else
			ExpandProduct(Orders, St.Id, Neighbors);
		for(const std::int64_t N : Neighbors) {
			const int NextDepth = St.Depth + 1;
			const VertexKind NextKind =
			    St.Kind == VertexKind::Customer ? VertexKind::Product : VertexKind::Customer;
			if(NextDepth >= Seg.MinHops && NextDepth <= Seg.MaxHops && NextKind == EndKind)
				Best[N] = Best.count(N) ? std::min(Best[N], NextDepth) : NextDepth;
			if(NextDepth >= Seg.MaxHops)
				continue;
			const std::uint64_t Key =
			    (static_cast<std::uint64_t>(NextKind) << 56) | static_cast<std::uint64_t>(N);
			if(Seen.count(Key))
				continue;
			Seen.insert(Key);
			Q.push_back({N, NextKind, NextDepth});
		}
	}
	return Best;
}

Database::Schema MatchVarLenSchema() {
	Database::Schema Sch;
	Database::Column A;
	A.Name = "start_id";
	A.DefaultValue = "TEXT";
	Sch.push_back(A);
	Database::Column B;
	B.Name = "end_id";
	B.DefaultValue = "TEXT";
	Sch.push_back(B);
	Database::Column C;
	C.Name = "path_length";
	C.DefaultValue = "TEXT";
	Sch.push_back(C);
	Database::Column D;
	D.Name = "path";
	D.DefaultValue = "TEXT";
	Sch.push_back(D);
	return Sch;
}

} // namespace

void RunGraphMatchLazyBulk(Database &Db, const GraphSpec &Spec, const ColumnarTable &Orders,
                           const GraphMatchBulkRequest &Req, std::uint64_t *RowsScannedOut) {
	if(RowsScannedOut)
		*RowsScannedOut = Orders.RowCount;
	if(!LabelPasses(Spec, Req.EdgeLabelFilter)) {
		Db.ReplaceTableContents(Req.ResultTable, MatchVarLenSchema(), {});
		return;
	}
	const std::int64_t Anchor = ParseAnchorId(Req.AnchorVertexId.empty() ? "1" : Req.AnchorVertexId);
	if(Req.Segments.empty()) {
		Db.ReplaceTableContents(Req.ResultTable, MatchVarLenSchema(), {});
		return;
	}
	std::unordered_set<std::int64_t> Frontier;
	Frontier.insert(Anchor);
	VertexKind Kind = VertexKind::Customer;
	std::unordered_map<std::int64_t, int> FinalDepth;
	for(std::size_t Si = 0; Si < Req.Segments.size(); ++Si) {
		const GraphMatchBulkSegment &Seg = Req.Segments[Si];
		const VertexKind EndKind =
		    (Seg.EndOnProduct || Si + 1 < Req.Segments.size()) ? VertexKind::Product : VertexKind::Customer;
		std::unordered_map<std::int64_t, int> Depths = RunSegmentBfs(Orders, Seg, Kind, Frontier, EndKind);
		Frontier.clear();
		for(const auto &[Id, Depth] : Depths) {
			Frontier.insert(Id);
			if(Si + 1 == Req.Segments.size())
				FinalDepth[Id] = Depth;
		}
		Kind = EndKind;
		if(Frontier.empty())
			break;
	}
	Database::Table Out;
	Out.reserve(std::min(Frontier.size(), static_cast<std::size_t>(Limits::MaxGraphVarLenResultRows)));
	const std::string StartText = Req.AnchorVertexId.empty() ? std::to_string(Anchor) : Req.AnchorVertexId;
	for(const std::int64_t EndId : Frontier) {
		if(Out.size() >= Limits::MaxGraphVarLenResultRows)
			break;
		Database::Item Row;
		Row["start_id"] = StartText;
		Row["end_id"] = std::to_string(EndId);
		const int Depth = FinalDepth.count(EndId) ? FinalDepth[EndId] : static_cast<int>(Req.Segments.size());
		Row["path_length"] = std::to_string(static_cast<int64_t>(Depth));
		Row["path"] = StartText + ">" + Row["end_id"];
		Out.push_back(std::move(Row));
	}
	Db.ReplaceTableContents(Req.ResultTable, MatchVarLenSchema(), std::move(Out));
}

} // namespace AstralDB
