#pragma once

#include <Database/Graph/Graph.hxx>
#include <Database/Storage/ColumnarStorage.hxx>

#include <cstdint>

namespace AstralDB {

struct GraphMatchBulkRequest {
	std::string GraphName;
	std::string ResultTable;
	std::string AnchorVertexId;
	std::string EdgeLabelFilter;
	std::vector<GraphMatchBulkSegment> Segments;
};

/** Multi-hop graph match over lazy bulk synthetic orders (no adjacency materialization). */
void RunGraphMatchLazyBulk(Database &Db, const GraphSpec &Spec, const ColumnarTable &Orders,
                           const GraphMatchBulkRequest &Req, std::uint64_t *RowsScannedOut = nullptr);

} // namespace AstralDB
