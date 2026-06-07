#pragma once

#include <Database/Storage/ColumnChunk.hxx>
#include <DS/ShardCompress.hxx>
#include <IO/ColumnShardConfig.hxx>

#include <string>
#include <vector>

namespace AstralDB {

/** Page-level LZ4/LZX wrapper on encoded chunk bytes. */
struct ChunkCompressor {
	static std::vector<std::byte> Compress(const ColumnChunk &Chunk, ColumnShardCompression Mode);
	static ColumnChunk Decompress(const std::byte *Data, std::size_t Size, ColumnShardCompression Mode);
};

} // namespace AstralDB
