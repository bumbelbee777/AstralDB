#pragma once

#include <cstddef>
#include <cstdint>

namespace AstralDB {
namespace SQL {

/** Opaque call sites so LTO cannot optimize across JIT page boundaries. */
std::size_t JitInvokeFilter(std::size_t (*Fn)(const int64_t *, std::size_t, int64_t, std::size_t *),
                            const int64_t *Values, std::size_t Count, int64_t Literal, std::size_t *Out);
int64_t JitInvokeSum(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count);
int64_t JitInvokeMin(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count);
int64_t JitInvokeMax(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count);

} // namespace SQL
} // namespace AstralDB
