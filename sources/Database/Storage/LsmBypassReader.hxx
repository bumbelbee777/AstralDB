#pragma once

#include <Database/Storage/ColumnView.hxx>
#include <Database/Storage/CompressedColumnStore.hxx>
#include <IO/ColumnShardConfig.hxx>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace AstralDB {

/** ASHD v1 layout flags (breaking evolution, same magic). */
enum class AshdLayoutFlags : uint8_t {
	None = 0,
	TypedChunks = 1 << 0,
	PartialAggFooter = 1 << 1,
	ChunkIndex = 1 << 2
};

inline AshdLayoutFlags operator|(AshdLayoutFlags A, AshdLayoutFlags B) {
	return static_cast<AshdLayoutFlags>(static_cast<uint8_t>(A) | static_cast<uint8_t>(B));
}

inline AshdLayoutFlags operator&(AshdLayoutFlags A, AshdLayoutFlags B) {
	return static_cast<AshdLayoutFlags>(static_cast<uint8_t>(A) & static_cast<uint8_t>(B));
}

struct AshdChunkIndexEntry {
	std::string Column;
	std::size_t PayloadOffset = 0;
	std::size_t PayloadSize = 0;
	ColumnEncoding Encoding = ColumnEncoding::LegacyText;
	std::size_t RowCount = 0;
};

struct AshdShardCatalog {
	ColumnShardCompression Compression = ColumnShardCompression::Lz4;
	AshdLayoutFlags Flags = AshdLayoutFlags::None;
	std::vector<AshdChunkIndexEntry> Chunks;
	std::vector<std::byte> PartialAggFooter;
};

/** Selective chunk decode from mmap shard without full-column decompress. */
class LsmBypassReader {
public:
	explicit LsmBypassReader(std::filesystem::path Path);

	[[nodiscard]] bool Valid() const noexcept { return Valid_; }
	[[nodiscard]] const AshdShardCatalog &Catalog() const noexcept { return Catalog_; }
	ColumnView OpenColumn(std::string_view Column) const;
	[[nodiscard]] std::size_t RowCount() const noexcept { return RowCount_; }

private:
	bool Valid_ = false;
	std::size_t RowCount_ = 0;
	AshdShardCatalog Catalog_;
	std::vector<std::byte> MmapCopy_;
};

} // namespace AstralDB
