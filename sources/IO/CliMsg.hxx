#pragma once

#include <string_view>

namespace AstralDB {
namespace CliMsg {

inline constexpr std::string_view VersionPrefix = "AstralDB ";
inline constexpr std::string_view UsageHint =
    "Usage: astraldb [options] [file.sql]\n"
    "  -q, --query SQL     Run a single query string\n"
    "  -f, --file PATH     Run SQL from file\n"
    "  --help              Show help\n";

} // namespace CliMsg
} // namespace AstralDB
