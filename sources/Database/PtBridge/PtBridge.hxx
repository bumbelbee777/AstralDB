#pragma once

#include <Database/Catalog/Triggers.hxx>
#include <filesystem>
#include <string>
#include <string_view>

namespace AstralDB {

class Database;
class Logger;

/** Bridge between the storage engine and SQL-layer procedure/trigger compilation + execution. */
namespace PtBridge {

void CacheProcedureFromDefinition(Database &Db, std::string_view ProcedureName, std::string BodySql, bool OrReplace,
                                  std::string SourceDialect, std::string_view ExceptionHandlersJson,
                                  std::string_view ControlFlowJson);

void DropProcedureFromCatalog(const std::filesystem::path &SessionDbPath, std::string_view ProcedureName,
                              bool IfExists);

void ReplayWalCacheProcedure(Database &Db, std::string_view ProcedureName, std::string BodySql,
                             std::string ControlFlowBlob = {});

void ReplayWalDropProcedureFromCatalog(const std::filesystem::path &SessionDbPath, std::string_view ProcedureName);

StoredTriggerEntry CacheTriggerFromDefinition(Database &Db, StoredTriggerEntry Spec, bool OrReplace);

void DropTriggerFromCatalog(const std::filesystem::path &SessionDbPath, std::string_view TriggerName, bool IfExists);

void ReplayWalCacheTrigger(Database &Db, StoredTriggerEntry Spec);

void ReplayWalDropTriggerFromCatalog(const std::filesystem::path &SessionDbPath, std::string_view TriggerName);

} // namespace PtBridge

} // namespace AstralDB
