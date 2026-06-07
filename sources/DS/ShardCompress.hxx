#pragma once

#include <IO/ColumnShardConfig.hxx>
#include <string>
#include <string_view>

namespace AstralDB {
namespace DS {

/** Compress shard payload; prefers LZX on Windows when requested. */
std::string ShardCompress(std::string_view Plain, ColumnShardCompression Mode);
std::string ShardDecompress(std::string_view Blob, ColumnShardCompression Mode);

} // namespace DS
} // namespace AstralDB
