#pragma once

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Database.hxx>

#include <string>

namespace AstralDB {

/** Adaptive hash join: simple, parallel, or radix-partitioned by build size. */
void HashInnerJoinEqualityParallel(Database::Table &Result, const Database::Table &Left, const Database::Table &Right,
                                   const std::string &LeftCol, const std::string &RightCol);

bool TryColumnarInnerJoinEqualityParallel(const ColumnarTable &Left, const ColumnarTable &Right,
                                          const std::string &LeftCol, const std::string &RightCol, RowTable &Result);

} // namespace AstralDB
