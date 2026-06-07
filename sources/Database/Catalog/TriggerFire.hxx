#pragma once

#include <Database/Catalog/Triggers.hxx>
#include <string_view>
#include <unordered_map>

namespace AstralDB {

class Database;

/** Fire enabled triggers for \a Table / \a Timing / \a Event . When \a MutexAlreadyHeld is true the caller
 *  holds \c DbMutex_ exclusively and trigger bodies may invoke the query VM without deadlocking. */
void FireTriggersAssumeLocked(Database &Db, std::string_view Table, TriggerTiming Timing, TriggerEvent Event,
                              const std::unordered_map<std::string, std::string> *OldRow,
                              const std::unordered_map<std::string, std::string> *NewRow, bool MutexAlreadyHeld);

} // namespace AstralDB
