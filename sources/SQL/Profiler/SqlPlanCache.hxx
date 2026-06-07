#pragma once

#include <SQL/Bytecode/Bytecode.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AstralDB {
class Database;
class Logger;

namespace SQL {

/** Session-local LRU cache of parse+optimize bytecode keyed by SQL text, opt level, and catalog fingerprint. */
namespace SqlPlanCache {

[[nodiscard]] bool Enabled() noexcept;

[[nodiscard]] uint64_t CatalogFingerprint(const Database *Db) noexcept;

[[nodiscard]] std::optional<CompiledBytecode> Lookup(std::string_view Sql, OptimizationLevel OptLevel,
                                                     uint64_t CatalogFp) noexcept;

void Store(std::string_view Sql, OptimizationLevel OptLevel, uint64_t CatalogFp, CompiledBytecode Compiled) noexcept;

/** Compile on miss; returns cached bytecode on hit. */
[[nodiscard]] CompiledBytecode LookupOrCompile(std::string_view Sql, Logger *Log, OptimizationLevel OptLevel,
                                                     const Database *Db) noexcept;

void Clear() noexcept;

[[nodiscard]] uint64_t HitsForTests() noexcept;
[[nodiscard]] uint64_t MissesForTests() noexcept;

} // namespace SqlPlanCache

} // namespace SQL
} // namespace AstralDB
