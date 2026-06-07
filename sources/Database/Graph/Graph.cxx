#include <Database/Graph/Graph.hxx>
#include <Database/Database.hxx>
#include <IO/Error.hxx>
#include <IO/Limits.hxx>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace AstralDB {
namespace {

[[noreturn]] void FailGraph(std::string Message) {
	throw std::runtime_error(Err::Prefixed("graph", std::move(Message)));
}

std::string CellOrEmpty(const GraphRow &Row, const std::string &Col) {
	auto It = Row.find(Col);
	return It != Row.end() ? It->second : std::string();
}

bool EdgePassesFilter(const GraphSpec &Spec, const GraphRow &Row) {
	if(Spec.ProjectionEdgeFilter.empty() && Spec.EdgeLabelCol.empty())
		return true;
	if(Spec.EdgeLabelCol.empty())
		return true;
	const std::string Lbl = CellOrEmpty(Row, Spec.EdgeLabelCol);
	if(!Spec.ProjectionEdgeFilter.empty())
		return Lbl == Spec.ProjectionEdgeFilter;
	return true;
}

bool EdgePassesMatchFilter(const GraphSpec &Spec, const GraphArc &Arc, const std::string &LabelFilter) {
	if(LabelFilter.empty())
		return true;
	if(Spec.EdgeLabelCol.empty())
		return Arc.Label == LabelFilter;
	return Arc.Label == LabelFilter;
}

std::optional<double> ParseWeight(const std::string &Raw) {
	if(Raw.empty())
		return std::nullopt;
	try {
		const double V = std::stod(Raw);
		if(!std::isfinite(V) || V < 0.0)
			return std::nullopt;
		return V;
	} catch(...) {
		return std::nullopt;
	}
}

void AppendArc(std::unordered_map<std::string, std::vector<GraphArc>> &Map, const std::string &Src, GraphArc Arc) {
	Map[Src].push_back(std::move(Arc));
}

const std::vector<GraphArc> &OutArcs(const GraphAdjacency &Adj, const std::string &Id) {
	static const std::vector<GraphArc> Empty;
	const auto It = Adj.Out.find(Id);
	return It != Adj.Out.end() ? It->second : Empty;
}

const std::vector<GraphArc> &InArcs(const GraphAdjacency &Adj, const std::string &Id) {
	static const std::vector<GraphArc> Empty;
	const auto It = Adj.In.find(Id);
	return It != Adj.In.end() ? It->second : Empty;
}

const std::vector<GraphArc> &ForwardArcs(const GraphAdjacency &Adj, const GraphSpec &Spec, const std::string &Id) {
	return Spec.Undirected ? OutArcs(Adj, Id) : OutArcs(Adj, Id);
}

const std::vector<GraphArc> &MatchArcs(const GraphAdjacency &Adj, const GraphSpec &Spec, const std::string &Id,
                                       bool Reverse) {
	if(Reverse)
		return InArcs(Adj, Id);
	if(Spec.Undirected)
		return OutArcs(Adj, Id);
	return OutArcs(Adj, Id);
}

std::vector<std::string> CollectVertexIds(const GraphSpec &Spec, const GraphAdjacency &Adj,
                                          const GraphEdgeTable &VertexRows) {
	std::unordered_set<std::string> Seen;
	for(const GraphRow &Row : VertexRows) {
		const std::string Id = CellOrEmpty(Row, Spec.VertexIdCol);
		if(!Id.empty())
			Seen.insert(Id);
	}
	for(const auto &[Id, _] : Adj.Out)
		(void)_, Seen.insert(Id);
	for(const auto &[Id, Arcs] : Adj.Out)
		for(const GraphArc &A : Arcs)
			Seen.insert(A.Dst);
	std::vector<std::string> Out(Seen.begin(), Seen.end());
	std::sort(Out.begin(), Out.end());
	return Out;
}

std::string JoinPath(const std::vector<std::string> &Nodes) {
	std::ostringstream O;
	for(size_t I = 0; I < Nodes.size(); ++I) {
		if(I)
			O << ',';
		O << Nodes[I];
	}
	return O.str();
}

} // namespace

void BuildGraphAdjacencyFromEdgeRows(const GraphSpec &Spec, const GraphEdgeTable &EdgeRows, GraphAdjacency &Out) {
	Out.Out.clear();
	Out.In.clear();
	for(const GraphRow &Row : EdgeRows) {
		if(!EdgePassesFilter(Spec, Row))
			continue;
		const std::string Src = CellOrEmpty(Row, Spec.EdgeSrcCol);
		const std::string Dst = CellOrEmpty(Row, Spec.EdgeDstCol);
		if(Src.empty() || Dst.empty())
			continue;
		GraphArc Arc;
		Arc.Dst = Dst;
		if(!Spec.EdgeLabelCol.empty())
			Arc.Label = CellOrEmpty(Row, Spec.EdgeLabelCol);
		if(!Spec.EdgeWeightCol.empty())
			Arc.Weight = CellOrEmpty(Row, Spec.EdgeWeightCol);
		AppendArc(Out.Out, Src, Arc);
		AppendArc(Out.In, Dst, GraphArc{Src, Arc.Label, Arc.Weight});
		if(Spec.Undirected) {
			GraphArc Rev;
			Rev.Dst = Src;
			Rev.Label = Arc.Label;
			Rev.Weight = Arc.Weight;
			AppendArc(Out.Out, Dst, std::move(Rev));
			AppendArc(Out.In, Src, GraphArc{Dst, Arc.Label, Arc.Weight});
		}
	}
}

void GraphAdjacencyInsertEdge(GraphAdjacency &Adj, const GraphSpec &Spec, const GraphRow &Row) {
	if(!EdgePassesFilter(Spec, Row))
		return;
	const std::string Src = CellOrEmpty(Row, Spec.EdgeSrcCol);
	const std::string Dst = CellOrEmpty(Row, Spec.EdgeDstCol);
	if(Src.empty() || Dst.empty())
		return;
	GraphArc Arc;
	Arc.Dst = Dst;
	if(!Spec.EdgeLabelCol.empty())
		Arc.Label = CellOrEmpty(Row, Spec.EdgeLabelCol);
	if(!Spec.EdgeWeightCol.empty())
		Arc.Weight = CellOrEmpty(Row, Spec.EdgeWeightCol);
	AppendArc(Adj.Out, Src, Arc);
	AppendArc(Adj.In, Dst, GraphArc{Src, Arc.Label, Arc.Weight});
	if(Spec.Undirected) {
		GraphArc Rev;
		Rev.Dst = Src;
		Rev.Label = Arc.Label;
		Rev.Weight = Arc.Weight;
		AppendArc(Adj.Out, Dst, std::move(Rev));
		AppendArc(Adj.In, Src, GraphArc{Dst, Arc.Label, Arc.Weight});
	}
}

namespace {

Database::Schema TraverseResultSchema() {
	Database::Schema Sch;
	Database::Column IdCol;
	IdCol.Name = "vertex_id";
	IdCol.DefaultValue = "TEXT";
	Sch.push_back(IdCol);
	Database::Column DepthCol;
	DepthCol.Name = "depth";
	DepthCol.DefaultValue = "INT";
	Sch.push_back(DepthCol);
	return Sch;
}

Database::Schema MatchOneHopSchema(const GraphSpec &Spec) {
	Database::Schema Sch;
	Database::Column SrcCol;
	SrcCol.Name = "src_id";
	SrcCol.DefaultValue = "TEXT";
	Sch.push_back(SrcCol);
	Database::Column DstCol;
	DstCol.Name = "dst_id";
	DstCol.DefaultValue = "TEXT";
	Sch.push_back(DstCol);
	if(!Spec.EdgeLabelCol.empty()) {
		Database::Column Lbl;
		Lbl.Name = "edge_label";
		Lbl.DefaultValue = "TEXT";
		Sch.push_back(Lbl);
	}
	return Sch;
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
	Database::Column L;
	L.Name = "path_length";
	L.DefaultValue = "INT";
	Sch.push_back(L);
	Database::Column P;
	P.Name = "path";
	P.DefaultValue = "TEXT";
	Sch.push_back(P);
	return Sch;
}

Database::Schema ShortestPathSchema() {
	Database::Schema Sch;
	Database::Column Found;
	Found.Name = "found";
	Found.DefaultValue = "INT";
	Sch.push_back(Found);
	Database::Column Len;
	Len.Name = "path_length";
	Len.DefaultValue = "INT";
	Sch.push_back(Len);
	Database::Column P;
	P.Name = "path";
	P.DefaultValue = "TEXT";
	Sch.push_back(P);
	Database::Column Pos;
	Pos.Name = "position";
	Pos.DefaultValue = "INT";
	Sch.push_back(Pos);
	Database::Column Vid;
	Vid.Name = "vertex_id";
	Vid.DefaultValue = "TEXT";
	Sch.push_back(Vid);
	return Sch;
}

Database::Schema PageRankSchema() {
	Database::Schema Sch;
	Database::Column Id;
	Id.Name = "vertex_id";
	Id.DefaultValue = "TEXT";
	Sch.push_back(Id);
	Database::Column R;
	R.Name = "rank";
	R.DefaultValue = "TEXT";
	Sch.push_back(R);
	return Sch;
}

void EmitVarLenFromSource(const GraphSpec &Spec, const GraphAdjacency &Adj, const GraphMatchRequest &Req,
                          const std::string &Source, Database::Table &Out) {
	if(Req.MinHops > Req.MaxHops)
		return;
	if(static_cast<std::uint64_t>(Req.MaxHops) > Limits::MaxGraphTraverseDepth)
		FailGraph("variable-length path exceeds hop limit");

	struct State {
		std::string Cur;
		std::vector<std::string> Path;
		int64_t Depth = 0;
	};
	std::deque<State> Q;
	Q.push_back({Source, {Source}, 0});
	std::unordered_set<std::string> SeenState;

	while(!Q.empty() && Out.size() < Limits::MaxGraphVarLenResultRows) {
		State St = std::move(Q.front());
		Q.pop_front();
		if(St.Depth >= Req.MaxHops)
			continue;
		for(const GraphArc &Arc : MatchArcs(Adj, Spec, St.Cur, Req.Reverse)) {
			if(!EdgePassesMatchFilter(Spec, Arc, Req.EdgeLabelFilter))
				continue;
			if(Arc.Dst == Source && St.Depth > 0)
				continue;
			std::vector<std::string> NextPath = St.Path;
			NextPath.push_back(Arc.Dst);
			const int64_t NextDepth = St.Depth + 1;
			const std::string StateKey = Arc.Dst + "#" + std::to_string(NextDepth);
			if(SeenState.count(StateKey))
				continue;
			SeenState.insert(StateKey);
			if(NextDepth >= Req.MinHops) {
				Database::Item Row;
				Row["start_id"] = Source;
				Row["end_id"] = Arc.Dst;
				Row["path_length"] = std::to_string(NextDepth);
				Row["path"] = JoinPath(NextPath);
				Out.push_back(std::move(Row));
			}
			if(NextDepth < Req.MaxHops)
				Q.push_back({Arc.Dst, std::move(NextPath), NextDepth});
		}
	}
}

} // namespace

void RunGraphTraverse(Database &Db, const GraphSpec &Spec, const GraphAdjacency &Adj,
                      const GraphTraverseRequest &Req) {
	(void)Spec;
	if(Req.MaxDepth < 0)
		FailGraph("GRAPH TRAVERSE depth must be non-negative");
	if(static_cast<std::uint64_t>(Req.MaxDepth) > Limits::MaxGraphTraverseDepth)
		FailGraph("GRAPH TRAVERSE depth exceeds limit");

	struct Node {
		std::string Id;
		int64_t Depth;
	};
	std::deque<Node> Queue;
	std::unordered_set<std::string> Seen;
	Database::Table Out;
	Seen.insert(Req.StartVertexId);
	Queue.push_back({Req.StartVertexId, 0});
	Database::Item Seed;
	Seed["vertex_id"] = Req.StartVertexId;
	Seed["depth"] = "0";
	Out.push_back(std::move(Seed));

	auto VisitNeighbor = [&](const std::string &Neighbor, int64_t NextDepth) {
		if(Seen.count(Neighbor))
			return;
		Seen.insert(Neighbor);
		Database::Item Row;
		Row["vertex_id"] = Neighbor;
		Row["depth"] = std::to_string(NextDepth);
		Out.push_back(std::move(Row));
		if(NextDepth < Req.MaxDepth)
			Queue.push_back({Neighbor, NextDepth});
	};

	if(Req.Mode == GraphTraverseMode::Bfs) {
		while(!Queue.empty()) {
			const Node Cur = Queue.front();
			Queue.pop_front();
			for(const GraphArc &Arc : ForwardArcs(Adj, Spec, Cur.Id))
				VisitNeighbor(Arc.Dst, Cur.Depth + 1);
		}
	} else {
		while(!Queue.empty()) {
			const Node Cur = Queue.back();
			Queue.pop_back();
			for(const GraphArc &Arc : ForwardArcs(Adj, Spec, Cur.Id))
				VisitNeighbor(Arc.Dst, Cur.Depth + 1);
		}
	}
	Db.ReplaceTableContents(Req.ResultTable, TraverseResultSchema(), std::move(Out));
}

void RunGraphMatch(Database &Db, const GraphSpec &Spec, const GraphAdjacency &Adj, const GraphMatchRequest &Req,
                   const GraphEdgeTable &Edges) {
	(void)Edges;
	if(Req.MinHops == 1 && Req.MaxHops == 1) {
		Database::Table Out;
		for(const auto &[Src, Arcs] : Adj.Out) {
			if(Req.Reverse)
				continue;
			if(!Req.AnchorVertexId.empty() && Src != Req.AnchorVertexId)
				continue;
			for(const GraphArc &Arc : Arcs) {
				if(!EdgePassesMatchFilter(Spec, Arc, Req.EdgeLabelFilter))
					continue;
				Database::Item RowOut;
				RowOut["src_id"] = Src;
				RowOut["dst_id"] = Arc.Dst;
				if(!Spec.EdgeLabelCol.empty())
					RowOut["edge_label"] = Arc.Label;
				Out.push_back(std::move(RowOut));
			}
		}
		if(Req.Reverse) {
			for(const auto &[Dst, Arcs] : Adj.In) {
				if(!Req.AnchorVertexId.empty() && Dst != Req.AnchorVertexId)
					continue;
				for(const GraphArc &Arc : Arcs) {
					if(!EdgePassesMatchFilter(Spec, Arc, Req.EdgeLabelFilter))
						continue;
					Database::Item RowOut;
					RowOut["src_id"] = Arc.Dst;
					RowOut["dst_id"] = Dst;
					if(!Spec.EdgeLabelCol.empty())
						RowOut["edge_label"] = Arc.Label;
					Out.push_back(std::move(RowOut));
				}
			}
		}
		Db.ReplaceTableContents(Req.ResultTable, MatchOneHopSchema(Spec), std::move(Out));
		return;
	}

	Database::Table Out;
	if(!Req.AnchorVertexId.empty()) {
		EmitVarLenFromSource(Spec, Adj, Req, Req.AnchorVertexId, Out);
	} else {
		const GraphEdgeTable EmptyVerts;
		std::vector<std::string> Sources = CollectVertexIds(Spec, Adj, EmptyVerts);
		if(Sources.size() > Limits::MaxGraphVarLenSourceVertices)
			FailGraph("variable-length MATCH without FROM requires a smaller graph (use FROM start_id)");
		for(const std::string &Src : Sources)
			EmitVarLenFromSource(Spec, Adj, Req, Src, Out);
	}
	Db.ReplaceTableContents(Req.ResultTable, MatchVarLenSchema(), std::move(Out));
}

void RunGraphShortestPath(Database &Db, const GraphSpec &Spec, const GraphAdjacency &Adj,
                          const GraphShortestPathRequest &Req) {
	if(Req.Weighted && Spec.EdgeWeightCol.empty())
		FailGraph("GRAPH SHORTEST PATH WEIGHTED requires CREATE GRAPH … WEIGHT (column)");

	Database::Table Out;
	const std::string &From = Req.FromVertexId;
	const std::string &To = Req.ToVertexId;

	if(!Req.Weighted) {
		std::unordered_map<std::string, std::string> Parent;
		std::unordered_map<std::string, int64_t> Dist;
		std::deque<std::string> Q;
		Dist[From] = 0;
		Q.push_back(From);
		while(!Q.empty()) {
			const std::string Cur = Q.front();
			Q.pop_front();
			if(Cur == To)
				break;
			for(const GraphArc &Arc : ForwardArcs(Adj, Spec, Cur)) {
				if(Dist.count(Arc.Dst))
					continue;
				Dist[Arc.Dst] = Dist[Cur] + 1;
				Parent[Arc.Dst] = Cur;
				Q.push_back(Arc.Dst);
			}
		}
		if(!Dist.count(To)) {
			Database::Item Row;
			Row["found"] = "0";
			Row["path_length"] = "0";
			Row["path"] = "";
			Row["position"] = "0";
			Row["vertex_id"] = "";
			Out.push_back(std::move(Row));
			Db.ReplaceTableContents(Req.ResultTable, ShortestPathSchema(), std::move(Out));
			return;
		}
		std::vector<std::string> Path;
		for(std::string V = To;;) {
			Path.push_back(V);
			if(V == From)
				break;
			V = Parent.at(V);
		}
		std::reverse(Path.begin(), Path.end());
		for(size_t I = 0; I < Path.size(); ++I) {
			Database::Item Row;
			Row["found"] = "1";
			Row["path_length"] = std::to_string(static_cast<int64_t>(Path.size() - 1));
			Row["path"] = JoinPath(Path);
			Row["position"] = std::to_string(static_cast<int64_t>(I));
			Row["vertex_id"] = Path[I];
			Out.push_back(std::move(Row));
		}
		Db.ReplaceTableContents(Req.ResultTable, ShortestPathSchema(), std::move(Out));
		return;
	}

	using Node = std::pair<double, std::string>;
	std::priority_queue<Node, std::vector<Node>, std::greater<Node>> Pq;
	std::unordered_map<std::string, double> Best;
	std::unordered_map<std::string, std::string> Parent;
	Best[From] = 0.0;
	Pq.push({0.0, From});
	while(!Pq.empty()) {
		const auto [Cost, Cur] = Pq.top();
		Pq.pop();
		if(Cost > Best[Cur])
			continue;
		if(Cur == To)
			break;
		for(const GraphArc &Arc : ForwardArcs(Adj, Spec, Cur)) {
			const auto W = ParseWeight(Arc.Weight);
			if(!W)
				continue;
			const double Next = Cost + *W;
			if(Best.count(Arc.Dst) && Best[Arc.Dst] <= Next)
				continue;
			Best[Arc.Dst] = Next;
			Parent[Arc.Dst] = Cur;
			Pq.push({Next, Arc.Dst});
		}
	}
	if(!Best.count(To)) {
		Database::Item Row;
		Row["found"] = "0";
		Row["path_length"] = "0";
		Row["path"] = "";
		Row["position"] = "0";
		Row["vertex_id"] = "";
		Out.push_back(std::move(Row));
		Db.ReplaceTableContents(Req.ResultTable, ShortestPathSchema(), std::move(Out));
		return;
	}
	std::vector<std::string> Path;
	for(std::string V = To;;) {
		Path.push_back(V);
		if(V == From)
			break;
		V = Parent.at(V);
	}
	std::reverse(Path.begin(), Path.end());
	for(size_t I = 0; I < Path.size(); ++I) {
		Database::Item Row;
		Row["found"] = "1";
		Row["path_length"] = std::to_string(Best[To]);
		Row["path"] = JoinPath(Path);
		Row["position"] = std::to_string(static_cast<int64_t>(I));
		Row["vertex_id"] = Path[I];
		Out.push_back(std::move(Row));
	}
	Db.ReplaceTableContents(Req.ResultTable, ShortestPathSchema(), std::move(Out));
}

void RunGraphPageRank(Database &Db, const GraphSpec &Spec, const GraphAdjacency &Adj, const GraphPageRankRequest &Req,
                      const GraphEdgeTable &VertexRows) {
	(void)Db;
	const std::vector<std::string> Vertices = CollectVertexIds(Spec, Adj, VertexRows);
	if(Vertices.empty()) {
		Db.ReplaceTableContents(Req.ResultTable, PageRankSchema(), {});
		return;
	}
	const double Damping = static_cast<double>(Req.DampingMillis) / 1000.0;
	if(Damping <= 0.0 || Damping >= 1.0)
		FailGraph("PAGERANK DAMPING must be between 0 and 1 (use DAMPING 0.85)");
	const int64_t IterCap = (std::min)(Req.Iterations, static_cast<int64_t>(Limits::MaxGraphPageRankIterations));
	if(IterCap <= 0)
		FailGraph("PAGERANK ITERATIONS must be positive");

	const double N = static_cast<double>(Vertices.size());
	const double Base = (1.0 - Damping) / N;
	std::unordered_map<std::string, double> Rank;
	std::unordered_map<std::string, double> OutDeg;
	for(const std::string &V : Vertices) {
		Rank[V] = 1.0 / N;
		size_t Deg = OutArcs(Adj, V).size();
		if(Deg == 0)
			Deg = 1;
		OutDeg[V] = static_cast<double>(Deg);
	}

	for(int64_t It = 0; It < IterCap; ++It) {
		std::unordered_map<std::string, double> Next;
		for(const std::string &V : Vertices)
			Next[V] = Base;
		for(const std::string &U : Vertices) {
			const double Share = Damping * Rank[U] / OutDeg[U];
			for(const GraphArc &Arc : OutArcs(Adj, U))
				Next[Arc.Dst] += Share;
		}
		Rank = std::move(Next);
	}

	Database::Table Out;
	Out.reserve(Vertices.size());
	for(const std::string &V : Vertices) {
		Database::Item Row;
		Row["vertex_id"] = V;
		Row["rank"] = std::to_string(Rank[V]);
		Out.push_back(std::move(Row));
	}
	Db.ReplaceTableContents(Req.ResultTable, PageRankSchema(), std::move(Out));
}

} // namespace AstralDB
