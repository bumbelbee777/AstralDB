#pragma once

#include <SQL/BytecodeTriggers.hxx>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AstralDB {

class Database;

/** Fire enabled triggers for \a Table / \a Timing / \a Event . When \a MutexAlreadyHeld is true the caller
 *  holds \c DbMutex_ exclusively and trigger bodies may invoke the SQL VM without deadlocking. */
void FireTriggersAssumeLocked(Database &Db, std::string_view Table, SQL::TriggerTiming Timing, SQL::TriggerEvent Event,
                              const std::unordered_map<std::string, std::string> *OldRow,
                              const std::unordered_map<std::string, std::string> *NewRow, bool MutexAlreadyHeld);

} // namespace AstralDB
