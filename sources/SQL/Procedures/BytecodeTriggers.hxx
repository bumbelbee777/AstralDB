#pragma once

#include <SQL/BytecodeFormat.hxx>
#include <SQL/Bytecode.hxx>
#include <IO/Logger.hxx>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AstralDB {
class Database;
namespace SQL {

enum class TriggerTiming { Before, After, InsteadOf };
enum class TriggerEvent { Insert, Update, Delete };

const char *TriggerTimingTag(TriggerTiming T);
const char *TriggerEventTag(TriggerEvent E);
std::optional<TriggerTiming> ParseTriggerTiming(std::string_view S);
std::optional<TriggerEvent> ParseTriggerEvent(std::string_view S);

struct TriggerBytecodeMeta {
	std::size_t InstructionCount = 0;
	std::vector<std::string> CalledProcedures;
};

struct TriggerFireRecord {
	std::string TriggerName;
	std::string TableName;
	std::string Timing;
	std::string Event;
	std::uint64_t FiredAtUnixMs = 0;
	bool Ok = true;
	std::string Detail;
};

struct StoredTriggerEntry {
	std::string Name;
	std::string TableName;
	TriggerTiming Timing = TriggerTiming::After;
	TriggerEvent Event = TriggerEvent::Insert;
	bool ForEachRow = true;
	bool Enabled = true;
	/** \c procedure or \c inline */
	std::string ActionKind;
	std::string ProcedureName;
	std::string BodySql;
	std::filesystem::path AbcPath;
	std::filesystem::path SqlPath;
	std::string SourceHash;
	TriggerBytecodeMeta BytecodeMeta;
};

struct TriggerCatalog {
	std::filesystem::path CatalogPath;
	std::vector<StoredTriggerEntry> Triggers;
	/** table → trigger names (declaration order). */
	std::unordered_map<std::string, std::vector<std::string>> TableIndex;
	std::vector<TriggerFireRecord> RecentFires;
};

TriggerCatalog LoadTriggerCatalog(const std::filesystem::path &CatalogPath);
void SaveTriggerCatalog(const TriggerCatalog &Catalog);
void RebuildTriggerRelations(TriggerCatalog &Catalog);

std::filesystem::path DefaultTriggerCatalogPath(const std::filesystem::path &SessionDbPath);
std::filesystem::path DefaultTriggerCacheDir(const std::filesystem::path &SessionDbPath);

std::optional<StoredTriggerEntry> FindTrigger(const TriggerCatalog &Catalog, std::string_view Name);
std::vector<const StoredTriggerEntry *> TriggersForTableEvent(const TriggerCatalog &Catalog, std::string_view Table,
                                                              TriggerTiming Timing, TriggerEvent Event);

std::string HashTriggerSource(std::string_view BodySql);
CompiledBytecode CompileTriggerBody(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                    std::string_view BodySql);
LoadedAbcFile LoadTriggerBytecode(const StoredTriggerEntry &Entry);

StoredTriggerEntry CacheTriggerFromSql(TriggerCatalog &Catalog, const std::filesystem::path &SessionDbPath,
                                       StoredTriggerEntry Spec, Logger *Logger, OptimizationLevel OptLevel,
                                       const Database *CatalogDb, bool IfNotExists, bool OrReplace);

bool UnregisterTrigger(TriggerCatalog &Catalog, std::string_view Name);
void DropTriggerCacheFiles(const StoredTriggerEntry &Entry);

void AppendTriggerFireRecord(TriggerCatalog &Catalog, TriggerFireRecord Rec);

std::string FormatTriggerEntrySummary(const StoredTriggerEntry &Entry);
std::string FormatTriggerRelations(const TriggerCatalog &Catalog);
std::string FormatTriggerFireLog(const TriggerCatalog &Catalog);

/** Serialize registry for database snapshot trailer (reversible with main \c .db). */
std::string SerializeTriggerRegistry(const std::vector<StoredTriggerEntry> &Triggers);
bool DeserializeTriggerRegistry(std::string_view Blob, std::vector<StoredTriggerEntry> &Out);

} // namespace SQL
} // namespace AstralDB
