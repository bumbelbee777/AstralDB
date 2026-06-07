#pragma once

#include <Database/Catalog/Triggers.hxx>
#include <SQL/Bytecode/BytecodeFormat.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
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

using TriggerTiming = AstralDB::TriggerTiming;
using TriggerEvent = AstralDB::TriggerEvent;
using TriggerBytecodeMeta = AstralDB::TriggerBytecodeMeta;
using TriggerFireRecord = AstralDB::TriggerFireRecord;
using StoredTriggerEntry = AstralDB::StoredTriggerEntry;
using TriggerCatalog = AstralDB::TriggerCatalog;

using AstralDB::RebuildTriggerRelations;
using AstralDB::HashTriggerSource;
using AstralDB::FormatTriggerEntrySummary;
using AstralDB::FormatTriggerRelations;
using AstralDB::FormatTriggerFireLog;

std::vector<const StoredTriggerEntry *> TriggersForTableEvent(const TriggerCatalog &Catalog, std::string_view Table,
                                                              TriggerTiming Timing, TriggerEvent Event);

CompiledBytecode CompileTriggerBody(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                    std::string_view BodySql);
LoadedAbcFile LoadTriggerBytecode(const StoredTriggerEntry &Entry);

StoredTriggerEntry CacheTriggerFromSql(TriggerCatalog &Catalog, const std::filesystem::path &SessionDbPath,
                                       StoredTriggerEntry Spec, Logger *Logger, OptimizationLevel OptLevel,
                                       const Database *CatalogDb, bool IfNotExists, bool OrReplace);

bool UnregisterTrigger(TriggerCatalog &Catalog, std::string_view Name);
void DropTriggerCacheFiles(const StoredTriggerEntry &Entry);

} // namespace SQL
} // namespace AstralDB
