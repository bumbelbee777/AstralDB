#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AstralDB {
namespace TimeSeries {

enum class TruncUnit : int8_t { Year = 0, Month = 1, Day = 2, Hour = 3, Minute = 4, Second = 5 };

/** Packed YYYYMMDD from an ISO date prefix, or nullopt when invalid. */
std::optional<int> ParseIsoYmd(std::string_view S);

/** Unix epoch seconds from ISO date or date-time text (UTC, no TZ offset math). */
std::optional<int64_t> ParseEpochSeconds(std::string_view S);

std::string FormatIsoYmd(int Packed);
std::string FormatEpochSeconds(int64_t Epoch);

std::optional<TruncUnit> ParseTruncUnit(std::string_view Unit);
std::optional<int64_t> TruncateEpoch(int64_t Epoch, TruncUnit Unit);
std::optional<int64_t> TimeBucketEpoch(int64_t Epoch, int64_t BucketSeconds);

std::optional<int> IsoAddDays(int PackedYmd, int Delta);
std::optional<int64_t> EpochAddDays(int64_t Epoch, int Delta);
std::optional<int64_t> EpochAddSeconds(int64_t Epoch, int64_t Delta);

int EpochDiffDays(int64_t A, int64_t B);
int64_t EpochDiffSeconds(int64_t A, int64_t B);

} // namespace TimeSeries
} // namespace AstralDB
