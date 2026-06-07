#include <IO/ColumnShardConfig.hxx>

#include <cstdlib>
#include <cstring>

namespace AstralDB {
namespace {

const char *EnvGet(const char *Name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	return std::getenv(Name);
#pragma warning(pop)
#else
	return std::getenv(Name);
#endif
}

std::uint64_t EnvU64(const char *Name, std::uint64_t Default) {
	if(const char *V = EnvGet(Name)) {
		try {
			return static_cast<std::uint64_t>(std::stoull(V));
		} catch(...) {
		}
	}
	return Default;
}

unsigned EnvU32(const char *Name, unsigned Default) {
	return static_cast<unsigned>(EnvU64(Name, Default));
}

} // namespace

ColumnShardCompression ColumnShardConfig::ParseCompression(std::string_view Token) noexcept {
	if(Token == "off" || Token == "none" || Token == "0")
		return ColumnShardCompression::Off;
	if(Token == "lzx" || Token == "LZX")
		return ColumnShardCompression::Lzx;
	return ColumnShardCompression::Lz4;
}

ColumnShardConfig ColumnShardConfig::FromEnvironment() noexcept {
	ColumnShardConfig Cfg;
	Cfg.TtlSeconds = EnvU64("ASTRALDB_SHARD_TTL_SEC", Cfg.TtlSeconds);
	Cfg.SpillMinRows = EnvU64("ASTRALDB_SHARD_SPILL_MIN_ROWS", Cfg.SpillMinRows);
	if(const char *Cmp = EnvGet("ASTRALDB_SHARD_COMPRESS"))
		Cfg.Compression = ParseCompression(Cmp);
	Cfg.IocpDepth = EnvU32("ASTRALDB_SHARD_IOCP_DEPTH", Cfg.IocpDepth);
	Cfg.IocpReadAheadChunks = EnvU32("ASTRALDB_SHARD_IOCP_READAHEAD", Cfg.IocpReadAheadChunks);
	return Cfg;
}

const char *ColumnShardConfig::CompressionName() const noexcept {
	switch(Compression) {
	case ColumnShardCompression::Off:
		return "off";
	case ColumnShardCompression::Lzx:
		return "lzx";
	case ColumnShardCompression::Lz4:
	default:
		return "lz4";
	}
}

} // namespace AstralDB
