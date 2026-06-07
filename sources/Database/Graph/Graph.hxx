#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

class Database;

using GraphRow = std::unordered_map<std::string, std::string>;
using GraphEdgeTable = std::vector<GraphRow>;

struct GraphArc {
	std::string Dst;
	std::string Label;
	/** Optional edge weight as decimal text; empty when unweighted. */
	std::string Weight;
};

/** Directed (and optional reverse) adjacency for fast traversals. */
struct GraphAdjacency {
	std::unordered_map<std::string, std::vector<GraphArc>> Out;
	std::unordered_map<std::string, std::vector<GraphArc>> In;
};

/** Registered property-graph view over existing vertex/edge tables. */
struct GraphSpec {
	std::string Name;
	std::string VertexTable;
	std::string VertexIdCol;
	std::string EdgeTable;
	std::string EdgeSrcCol;
	std::string EdgeDstCol;
	std::string EdgeLabelCol;
	std::string EdgeWeightCol;
	bool Undirected = false;
	/** When set, this graph is a filtered view of \c ProjectionOf (same vertex/edge tables). */
	std::string ProjectionOf;
	std::string ProjectionEdgeFilter;
	/** Edge table is lazy bulk synthetic; adjacency is derived at match time. */
	bool LazyBulkEdges = false;
};

enum class GraphTraverseMode : std::uint8_t { Bfs = 0, Dfs = 1 };

struct GraphTraverseRequest {
	std::string GraphName;
	std::string StartVertexId;
	int64_t MaxDepth = 1;
	GraphTraverseMode Mode = GraphTraverseMode::Bfs;
	std::string ResultTable;
};

struct GraphMatchBulkSegment {
	int64_t MinHops = 1;
	int64_t MaxHops = 1;
	bool Reverse = false;
	/** When true, segment target vertex is the edge destination column (product id). */
	bool EndOnProduct = false;
};

struct GraphMatchRequest {
	std::string GraphName;
	std::string EdgeLabelFilter;
	int64_t MinHops = 1;
	int64_t MaxHops = 1;
	/** When non-empty, only expand from this vertex (required for large graphs). */
	std::string AnchorVertexId;
	bool Reverse = false;
	std::string ResultTable;
	/** Multi-hop pattern segments for lazy bulk graphs (empty = single-segment \c MinHops/\c MaxHops). */
	std::vector<GraphMatchBulkSegment> BulkSegments;
};

struct GraphShortestPathRequest {
	std::string GraphName;
	std::string FromVertexId;
	std::string ToVertexId;
	bool Weighted = false;
	std::string ResultTable;
};

struct GraphPageRankRequest {
	std::string GraphName;
	/** Damping in millis (850 = 0.85). */
	int64_t DampingMillis = 850;
	int64_t Iterations = 20;
	std::string ResultTable;
};

struct GraphProjectionRequest {
	std::string ProjectionName;
	std::string BaseGraphName;
	std::string EdgeLabelFilter;
};

void BuildGraphAdjacencyFromEdgeRows(const GraphSpec &Spec, const GraphEdgeTable &EdgeRows, GraphAdjacency &Out);
void GraphAdjacencyInsertEdge(GraphAdjacency &Adj, const GraphSpec &Spec, const GraphRow &Row);

void RunGraphTraverse(Database &Db, const GraphSpec &Spec, const GraphAdjacency &Adj, const GraphTraverseRequest &Req);
void RunGraphMatch(Database &Db, const GraphSpec &Spec, const GraphAdjacency &Adj, const GraphMatchRequest &Req,
                   const GraphEdgeTable &EdgeRows);
void RunGraphShortestPath(Database &Db, const GraphSpec &Spec, const GraphAdjacency &Adj,
                          const GraphShortestPathRequest &Req);
void RunGraphPageRank(Database &Db, const GraphSpec &Spec, const GraphAdjacency &Adj, const GraphPageRankRequest &Req,
                      const GraphEdgeTable &VertexRows);

} // namespace AstralDB
