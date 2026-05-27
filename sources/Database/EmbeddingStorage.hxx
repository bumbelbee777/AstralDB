#pragma once

#include <Database/EmbeddingCatalog.hxx>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
class Database;

void InstallEmbeddingCatalogEntryAssumeLocked(Database &Db, EmbeddingCatalogEntry Entry);
void InstallEmbeddingCatalogAssumeLocked(Database &Db, std::vector<EmbeddingCatalogEntry> Entries);
void ReplayWalEmbeddingRegisterAssumeLocked(Database &Db, EmbeddingCatalogEntry Entry);
void ReplayWalEmbeddingDropAssumeLocked(Database &Db, const std::string &Name);

std::string EmbeddingWalLineRegister(const EmbeddingCatalogEntry &Entry);
std::string EmbeddingWalLineDrop(const std::string &Name);
/** WAL tokens: ER, name, table, tokenCol, vecCol, isComplex, wireB64 */
EmbeddingCatalogEntry ParseEmbeddingWalRegisterTokens(const std::vector<std::string> &Tok);

void AppendEmbeddingCatalogSnapshotTrailer(
    std::string &RawData, const std::unordered_map<std::string, EmbeddingCatalogEntry> &Embeddings);
bool StripAndParseEmbeddingCatalogSnapshotTrailer(std::string &RawData,
                                                  std::vector<EmbeddingCatalogEntry> &OutEntries);

/** Build catalog entry by scanning \p TableName (caller holds \c DbMutex_). */
EmbeddingCatalogEntry BuildEmbeddingCatalogEntryFromTable(Database &Db, const std::string &Name,
                                                          const std::string &TableName,
                                                          const std::string &TokenColumn,
                                                          const std::string &VectorColumn);

} // namespace AstralDB
