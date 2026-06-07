#pragma once

#include <Database/Graph/Graph.hxx>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AstralDB {

class Database;

/** Serialize graph catalog for DB snapshot trailer (specs only; adjacency rebuilt from edge tables). */
void AppendGraphCatalogSnapshotTrailer(std::string &RawData,
                                       const std::unordered_map<std::string, std::pair<GraphSpec, GraphAdjacency>> &Graphs);

/** Parse trailer; returns false on corruption. Does not rebuild adjacency. */
bool StripAndParseGraphCatalogSnapshotTrailer(std::string &RawData, std::vector<GraphSpec> &OutSpecs);

/** Install one catalog entry and rebuild native adjacency from the edge table (\c DbMutex_ held). */
void RegisterGraphCatalogEntryAssumeLocked(Database &Db, GraphSpec Spec);

/** Rebuild full catalog from snapshot specs (\c DbMutex_ held). */
void InstallGraphCatalogAssumeLocked(Database &Db, std::vector<GraphSpec> Specs);

void ReplayWalGraphRegisterAssumeLocked(Database &Db, GraphSpec Spec);
void ReplayWalGraphDropAssumeLocked(Database &Db, const std::string &Name);
void ReplayWalGraphProjectionAssumeLocked(Database &Db, const GraphProjectionRequest &Req);

std::string GraphWalLineRegister(const GraphSpec &Spec);
std::string GraphWalLineDrop(const std::string &Name);
std::string GraphWalLineProjection(const GraphProjectionRequest &Req);

} // namespace AstralDB
