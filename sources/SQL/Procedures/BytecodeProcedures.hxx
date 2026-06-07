#pragma once

#include <SQL/Bytecode/BytecodeFormat.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Procedures/ProcedureParser.hxx>
#include <IO/Logger.hxx>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
class Database;
namespace SQL {

struct ProcedureBytecodeMeta {
	std::size_t InstructionCount = 0;
	std::size_t ExceptionHandlerCount = 0;
	bool HasExceptionHandlers = false;
	std::vector<std::string> ExceptionConditions;
	std::vector<std::string> CalledProcedures;
};

struct StoredProcedureEntry {
	std::string Name;
	std::filesystem::path AbcPath;
	std::filesystem::path SqlPath;
	std::string Description;
	/** \c plsql, \c plpgsql, or empty for standard AstralDB syntax. */
	std::string SourceDialect;
	std::string SourceSql;
	std::string SourceHash;
	std::vector<std::string> ReferencedTables;
	std::vector<std::string> DependsOn;
	std::vector<std::string> CalledBy;
	ProcedureBytecodeMeta BytecodeMeta;
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

/** Scan SQL text for \c CALL / \c EXEC / \c EXECUTE [PROCEDURE] invocations (case-insensitive). */
std::vector<std::string> ScanProcedureCallsInSql(std::string_view BodySql);

ProcedureBytecodeMeta AnalyzeProcedureBytecode(const Bytecode &Code);

CompiledBytecode CompileProcedureBody(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                      const LoweredProcedureBody &Body);

CompiledBytecode CompileProcedureBody(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                      std::string_view BodySql,
                                      const std::vector<ProcedureExceptionWhen> &ExceptionHandlers = {});

/** Compact APCF binary blob (preferred for WAL / bytecode operands). */
std::string EncodeProcedureControlBinary(const LoweredProcedureBody &Body);

LoweredProcedureBody DecodeProcedureControl(std::string_view Blob, std::string_view FallbackLinearSql = {});

/** Legacy JSON encoding (tests / external tools). */
std::string EncodeProcedureControlJson(const LoweredProcedureBody &Body);

LoweredProcedureBody DecodeProcedureControlJson(std::string_view Json, std::string_view FallbackLinearSql = {});

/** Stash structured procedure body for the next \c CacheProcedureFromSql (avoids JSON roundtrip). */
void StashLoweredProcedureBody(std::string_view Name, LoweredProcedureBody Body);

std::optional<LoweredProcedureBody> TakeStashedLoweredProcedureBody(std::string_view Name);

std::string EncodeExceptionHandlersJson(const std::vector<ProcedureExceptionWhen> &Handlers);

std::vector<ProcedureExceptionWhen> DecodeExceptionHandlersJson(std::string_view Json);

std::string HashProcedureSource(std::string_view BodySql);

/** Compile body, write \c .abc + \c .sql under cache dir, update catalog relations. */
StoredProcedureEntry CacheProcedureFromSql(ProcedureCatalog &Catalog, const std::filesystem::path &SessionDbPath,
                                           std::string Name, std::string BodySql, Logger *Logger,
                                           OptimizationLevel OptLevel, const Database *CatalogDb, bool IfNotExists,
                                           bool OrReplace = false, std::string SourceDialect = {},
                                           const std::vector<ProcedureExceptionWhen> &ExceptionHandlers = {},
                                           std::string_view ControlFlowJson = {});

void DropProcedureCacheFiles(const StoredProcedureEntry &Entry);

std::string FormatProcedureRelations(const ProcedureCatalog &Catalog);

std::string FormatProcedureEntrySummary(const StoredProcedureEntry &Entry);

} // namespace SQL
} // namespace AstralDB
