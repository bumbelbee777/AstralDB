#include <Database/GraphStorage.hxx>
#include <Database/Database.hxx>
#include <Database/WriteAheadLog.hxx>
#include <IO/Error.hxx>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace AstralDB {
namespace {

constexpr std::string_view kGraphSnapshotMarkerSv = "<<<ASTRAL_DB_GRAPHS>>>\n";

[[noreturn]] void FailGraphStore(std::string Message) {
	throw std::runtime_error(Err::Prefixed("graph store", std::move(Message)));
}

std::string EscapeField(std::string_view S) {
	std::string Out;
	Out.reserve(S.size());
	for(char C : S) {
		if(C == '|' || C == '\n' || C == '\r')
			Out.push_back('_');
		else
			Out.push_back(C);
	}
	return Out;
}

void WriteSpecLine(std::string &Raw, const GraphSpec &Spec) {
	Raw += EscapeField(Spec.Name);
	Raw.push_back('|');
	Raw += EscapeField(Spec.VertexTable);
	Raw.push_back('|');
	Raw += EscapeField(Spec.VertexIdCol);
	Raw.push_back('|');
	Raw += EscapeField(Spec.EdgeTable);
	Raw.push_back('|');
	Raw += EscapeField(Spec.EdgeSrcCol);
	Raw.push_back('|');
	Raw += EscapeField(Spec.EdgeDstCol);
	Raw.push_back('|');
	Raw += EscapeField(Spec.EdgeLabelCol);
	Raw.push_back('|');
	Raw += EscapeField(Spec.EdgeWeightCol);
	Raw.push_back('|');
	Raw += std::to_string(Spec.Undirected ? 1 : 0);
	Raw.push_back('|');
	Raw += EscapeField(Spec.ProjectionOf);
	Raw.push_back('|');
	Raw += EscapeField(Spec.ProjectionEdgeFilter);
	Raw.push_back('\n');
}

GraphSpec ParseSpecLine(std::string_view Line) {
	GraphSpec Spec;
	std::string_view Rem = Line;
	auto Take = [&]() -> std::string {
		const size_t P = Rem.find('|');
		std::string Out;
		if(P == std::string_view::npos) {
			Out.assign(Rem.begin(), Rem.end());
			Rem = {};
		} else {
			Out.assign(Rem.begin(), Rem.begin() + static_cast<std::ptrdiff_t>(P));
			Rem.remove_prefix(P + 1);
		}
		return Out;
	};
	Spec.Name = Take();
	Spec.VertexTable = Take();
	Spec.VertexIdCol = Take();
	Spec.EdgeTable = Take();
	Spec.EdgeSrcCol = Take();
	Spec.EdgeDstCol = Take();
	Spec.EdgeLabelCol = Take();
	Spec.EdgeWeightCol = Take();
	Spec.Undirected = Take() == "1";
	Spec.ProjectionOf = Take();
	Spec.ProjectionEdgeFilter = Take();
	return Spec;
}

} // namespace

std::string GraphWalLineRegister(const GraphSpec &Spec) {
	std::ostringstream O;
	O << "GR|" << EscapeField(Spec.Name) << '|' << EscapeField(Spec.VertexTable) << '|'
	  << EscapeField(Spec.VertexIdCol) << '|' << EscapeField(Spec.EdgeTable) << '|' << EscapeField(Spec.EdgeSrcCol)
	  << '|' << EscapeField(Spec.EdgeDstCol) << '|' << EscapeField(Spec.EdgeLabelCol) << '|'
	  << EscapeField(Spec.EdgeWeightCol) << '|' << (Spec.Undirected ? 1 : 0) << '|' << EscapeField(Spec.ProjectionOf)
	  << '|' << EscapeField(Spec.ProjectionEdgeFilter);
	return O.str();
}

std::string GraphWalLineDrop(const std::string &Name) {
	return std::string("GD|") + EscapeField(Name);
}

std::string GraphWalLineProjection(const GraphProjectionRequest &Req) {
	return std::string("GP|") + EscapeField(Req.ProjectionName) + '|' + EscapeField(Req.BaseGraphName) + '|' +
	       EscapeField(Req.EdgeLabelFilter);
}

void RegisterGraphCatalogEntryAssumeLocked(Database &Db, GraphSpec Spec) {
	if(Db.Tables_.find(Spec.VertexTable) == Db.Tables_.end())
		FailGraphStore("graph vertex table \"" + Spec.VertexTable + "\" does not exist.");
	if(Db.Tables_.find(Spec.EdgeTable) == Db.Tables_.end())
		FailGraphStore("graph edge table \"" + Spec.EdgeTable + "\" does not exist.");
	GraphAdjacency Adj;
	const Database::Table &EdgeRows = Db.Tables_.at(Spec.EdgeTable).RowsForRead(std::nullopt, false);
	BuildGraphAdjacencyFromEdgeRows(Spec, EdgeRows, Adj);
	Db.Graphs_[Spec.Name] = {Spec, std::move(Adj)};
	if(Spec.ProjectionOf.empty()) {
		auto &Owners = Db.GraphsByEdgeTable_[Spec.EdgeTable];
		if(std::find(Owners.begin(), Owners.end(), Spec.Name) == Owners.end())
			Owners.push_back(Spec.Name);
	} else {
		Db.GraphProjectionsByBase_[Spec.ProjectionOf].push_back(Spec.Name);
	}
}

void AppendGraphCatalogSnapshotTrailer(std::string &RawData,
                                       const std::unordered_map<std::string, std::pair<GraphSpec, GraphAdjacency>> &Graphs) {
	if(Graphs.empty())
		return;
	RawData.append(kGraphSnapshotMarkerSv.data(), kGraphSnapshotMarkerSv.size());
	RawData += std::to_string(Graphs.size());
	RawData.push_back('\n');
	for(const auto &[_, Pair] : Graphs)
		WriteSpecLine(RawData, Pair.first);
}

bool StripAndParseGraphCatalogSnapshotTrailer(std::string &RawData, std::vector<GraphSpec> &OutSpecs) {
	OutSpecs.clear();
	const size_t Mp = RawData.find(kGraphSnapshotMarkerSv.data(), 0, kGraphSnapshotMarkerSv.size());
	if(Mp == std::string::npos)
		return true;
	std::string_view Tail(RawData.data() + Mp + kGraphSnapshotMarkerSv.size(),
	                     RawData.size() - Mp - kGraphSnapshotMarkerSv.size());
	const size_t NL = Tail.find('\n');
	if(NL == std::string_view::npos)
		return false;
	std::uint64_t NV = 0;
	for(unsigned char Ch : Tail.substr(0, NL)) {
		if(Ch < '0' || Ch > '9')
			return false;
		NV = NV * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(NL + 1);
	OutSpecs.reserve(static_cast<size_t>(NV));
	for(std::uint64_t I = 0; I < NV; ++I) {
		const size_t Ln = Tail.find('\n');
		if(Ln == std::string_view::npos)
			return false;
		OutSpecs.push_back(ParseSpecLine(Tail.substr(0, Ln)));
		Tail = Tail.substr(Ln + 1);
	}
	RawData.erase(Mp);
	return true;
}

void InstallGraphCatalogAssumeLocked(Database &Db, std::vector<GraphSpec> Specs) {
	Db.Graphs_.clear();
	Db.GraphsByEdgeTable_.clear();
	Db.GraphProjectionsByBase_.clear();
	std::sort(Specs.begin(), Specs.end(), [](const GraphSpec &A, const GraphSpec &B) {
		return A.ProjectionOf.empty() > B.ProjectionOf.empty();
	});
	for(GraphSpec &Spec : Specs)
		RegisterGraphCatalogEntryAssumeLocked(Db, std::move(Spec));
}

void ReplayWalGraphRegisterAssumeLocked(Database &Db, GraphSpec Spec) {
	RegisterGraphCatalogEntryAssumeLocked(Db, std::move(Spec));
}

void ReplayWalGraphDropAssumeLocked(Database &Db, const std::string &Name) {
	const auto It = Db.Graphs_.find(Name);
	if(It == Db.Graphs_.end())
		return;
	const std::vector<std::string> Children = Db.GraphProjectionsByBase_[Name];
	for(const std::string &Child : Children)
		Db.Graphs_.erase(Child);
	Db.GraphProjectionsByBase_.erase(Name);
	const auto ProjIt = Db.GraphProjectionsByBase_.find(It->second.first.ProjectionOf);
	if(ProjIt != Db.GraphProjectionsByBase_.end()) {
		auto &V = ProjIt->second;
		V.erase(std::remove(V.begin(), V.end(), Name), V.end());
	}
	Db.Graphs_.erase(It);
	if(It->second.first.ProjectionOf.empty()) {
		const auto MapIt = Db.GraphsByEdgeTable_.find(It->second.first.EdgeTable);
		if(MapIt != Db.GraphsByEdgeTable_.end()) {
			auto &V = MapIt->second;
			V.erase(std::remove(V.begin(), V.end(), Name), V.end());
			if(V.empty())
				Db.GraphsByEdgeTable_.erase(MapIt);
		}
	}
}

void ReplayWalGraphProjectionAssumeLocked(Database &Db, const GraphProjectionRequest &Req) {
	if(Db.Graphs_.find(Req.BaseGraphName) == Db.Graphs_.end())
		FailGraphStore("projection base graph \"" + Req.BaseGraphName + "\" missing during replay.");
	GraphSpec Spec = Db.Graphs_.at(Req.BaseGraphName).first;
	Spec.Name = Req.ProjectionName;
	Spec.ProjectionOf = Req.BaseGraphName;
	Spec.ProjectionEdgeFilter = Req.EdgeLabelFilter;
	RegisterGraphCatalogEntryAssumeLocked(Db, std::move(Spec));
}

} // namespace AstralDB
