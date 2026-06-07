#pragma once

#include <string_view>

namespace AstralDB {
namespace ErrMsg {

inline constexpr std::string_view WalCorruptRecord = "Corrupt WAL line";
inline constexpr std::string_view WalRepairHint = "delete or repair";
inline constexpr std::string_view CliNoFile = "Cannot open query file (check path and permissions): ";
inline constexpr std::string_view CliEmptyFile = "Query file is empty: ";
inline constexpr std::string_view StorageEmptyName = "empty name.";

} // namespace ErrMsg
} // namespace AstralDB
