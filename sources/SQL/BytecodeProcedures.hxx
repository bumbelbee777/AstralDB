#pragma once

#include <SQL/BytecodeFormat.hxx>
#include <SQL/Bytecode.hxx>
#include <IO/Logger.hxx>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
class Database;
namespace SQL {

struct StoredProcedureEntry {
	std::string Name;
	std::filesystem::path AbcPath;
	std::filesystem::path SqlPath;
	std::string Description;
	std::string SourceSql;
	std::string SourceHash;
	std::vector<std::string> ReferencedTables;
	std::vector<std::string> DependsOn;
	std::vector<std::string> CalledBy;
};

struct ProcedureCatalog {
	std::filesystem::path CatalogPath;
	std::vector<StoredProcedureEntry> Procedures;
	/** Maps normalized \c .abc path → procedure names sharing that bytecode file. */
	std::unordered_map<std::string, std::vector<std::string>> AbcIndex;
};

ProcedureCatalog LoadProcedureCatalog(const std::filesystem::path &CatalogPath);

void SaveProcedureCatalog(const ProcedureCatalog &Catalog);

void RebuildProcedureRelations(ProcedureCatalog &Catalog);

void RegisterProcedure(ProcedureCatalog &Catalog, std::string Name, const std::filesystem::path &AbcPath,
                       std::string Description = {}, std::string SourceSql = {});

bool UnregisterProcedure(ProcedureCatalog &Catalog, std::string_view Name);

std::optional<StoredProcedureEntry> FindProcedure(const ProcedureCatalog &Catalog, std::string_view Name);

std::filesystem::path DefaultProcedureCatalogPath(const std::filesystem::path &SessionDbPath);

std::filesystem::path DefaultProcedureCacheDir(const std::filesystem::path &SessionDbPath);

LoadedAbcFile LoadProcedureBytecode(const StoredProcedureEntry &Entry, const ProcedureCatalog &Catalog);

/** Scan SQL text for \c CALL / \c EXECUTE PROCEDURE dependencies (case-insensitive). */
std::vector<std::string> ScanProcedureCallsInSql(std::string_view BodySql);

std::string HashProcedureSource(std::string_view BodySql);

/** Compile body, write \c .abc + \c .sql under cache dir, update catalog relations. */
StoredProcedureEntry CacheProcedureFromSql(ProcedureCatalog &Catalog, const std::filesystem::path &SessionDbPath,
                                             std::string Name, std::string BodySql, Logger *Logger,
                                             OptimizationLevel OptLevel, const Database *CatalogDb, bool IfNotExists);

void DropProcedureCacheFiles(const StoredProcedureEntry &Entry);

std::string FormatProcedureRelations(const ProcedureCatalog &Catalog);

std::string FormatProcedureEntrySummary(const StoredProcedureEntry &Entry);

} // namespace SQL
} // namespace AstralDB
