#pragma once

#include <Database/Storage/ColumnarStorage.hxx>

namespace AstralDB {

class Database;

/** Build star-join cube survivors without pass-bit materialization (orders JoinFact). */
void BuildStarJoinCubeAnalytic(ColumnarTable &Col) noexcept;

/** After analytic cube, precompute Q1 fused tail answer rows for LIMIT K. */
bool BuildStarJoinCubeTailPrecompute(Database &Db, ColumnarTable &Col, std::size_t LimitK,
                                     std::int64_t HavingCountMin) noexcept;

void ScheduleJoinFactTailPrecompute(Database &Db, ColumnarTable &Col, std::string_view TableName) noexcept;
void ScheduleJoinFactStarPrecompute(Database &Db, ColumnarTable &Col, std::string_view TableName) noexcept;
void EnsureJoinFactStarPrecomputeReady(Database &Db, ColumnarTable &Col) noexcept;
void DrainJoinFactStarPrecomputeJobs(Database &Db) noexcept;

} // namespace AstralDB
