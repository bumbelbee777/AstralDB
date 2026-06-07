#include <Database/Storage/ChunkCompressor.hxx>

#include <cstring>

namespace AstralDB {

std::vector<std::byte> ChunkCompressor::Compress(const ColumnChunk &Chunk, ColumnShardCompression Mode) {
	if(Mode == ColumnShardCompression::Off)
		return Chunk.Payload;
	const std::string Plain(reinterpret_cast<const char *>(Chunk.Payload.data()), Chunk.Payload.size());
	const std::string Packed = DS::ShardCompress(Plain, Mode);
	std::vector<std::byte> Out(Packed.size());
	std::memcpy(Out.data(), Packed.data(), Packed.size());
	return Out;
}

ColumnChunk ChunkCompressor::Decompress(const std::byte *Data, std::size_t Size, ColumnShardCompression Mode) {
	ColumnChunk Out;
	if(Mode == ColumnShardCompression::Off) {
		Out.Payload.assign(Data, Data + Size);
		return Out;
	}
	const std::string Packed(reinterpret_cast<const char *>(Data), Size);
	std::string Plain = DS::ShardDecompress(Packed, Mode);
	Out.Payload.assign(reinterpret_cast<const std::byte *>(Plain.data()),
	                   reinterpret_cast<const std::byte *>(Plain.data() + Plain.size()));
	return Out;
}

} // namespace AstralDB
