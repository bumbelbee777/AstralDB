#include <Database/Storage/LsmBypassReader.hxx>

#include <Database/Storage/ChunkCompressor.hxx>
#include <DS/ShardCompress.hxx>

#include <cstring>
#include <fstream>

namespace AstralDB {

namespace {
constexpr char kMagic[] = "ASHD";
constexpr std::size_t kHeaderSize = 8;
constexpr std::uint32_t kPafgMagic = 0x50414647u; // PAFG

bool ParsePartialAggFooter(const std::byte *Data, std::size_t Size, std::vector<std::byte> &OutFooter) {
	if(Size < 8)
		return false;
	std::uint32_t Magic = 0;
	std::uint16_t Version = 0;
	std::uint16_t ColumnCount = 0;
	std::memcpy(&Magic, Data, 4);
	if(Magic != kPafgMagic)
		return false;
	std::memcpy(&Version, Data + 4, 2);
	std::memcpy(&ColumnCount, Data + 6, 2);
	(void)Version;
	std::size_t Off = 8;
	for(std::uint16_t I = 0; I < ColumnCount && Off < Size; ++I) {
		const std::uint8_t NameLen = static_cast<std::uint8_t>(Data[Off++]);
		if(Off + NameLen + 2 > Size)
			break;
		Off += NameLen;
		std::uint16_t AggFlags = 0;
		std::memcpy(&AggFlags, Data + Off, 2);
		Off += 2;
		(void)AggFlags;
	}
	OutFooter.assign(Data, Data + Size);
	return true;
}

std::size_t FooterScanStart(const AshdShardCatalog &Catalog, std::size_t PayloadOff, std::size_t PayloadSize) {
	if((Catalog.Flags & AshdLayoutFlags::ChunkIndex) == AshdLayoutFlags::None || Catalog.Chunks.empty())
		return PayloadOff + PayloadSize;
	std::size_t MaxEnd = PayloadOff;
	for(const AshdChunkIndexEntry &E : Catalog.Chunks) {
		const std::size_t End = PayloadOff + E.PayloadOffset + E.PayloadSize;
		if(End > MaxEnd)
			MaxEnd = End;
	}
	return MaxEnd;
}

} // namespace

LsmBypassReader::LsmBypassReader(std::filesystem::path Path) {
	std::ifstream In(Path, std::ios::binary);
	if(!In)
		return;
	In.seekg(0, std::ios::end);
	const std::streamoff Sz = In.tellg();
	if(Sz <= static_cast<std::streamoff>(kHeaderSize))
		return;
	In.seekg(0, std::ios::beg);
	MmapCopy_.resize(static_cast<std::size_t>(Sz));
	In.read(reinterpret_cast<char *>(MmapCopy_.data()), Sz);
	if(!In)
		return;
	if(std::memcmp(MmapCopy_.data(), kMagic, 4) != 0)
		return;
	Catalog_.Compression = static_cast<ColumnShardCompression>(static_cast<unsigned char>(MmapCopy_[4]));
	const uint8_t F0 = static_cast<uint8_t>(MmapCopy_[5]);
	const uint8_t F1 = static_cast<uint8_t>(MmapCopy_[6]);
	const uint8_t F2 = static_cast<uint8_t>(MmapCopy_[7]);
	Catalog_.Flags = static_cast<AshdLayoutFlags>(F0 | (F1 << 1) | (F2 << 2));
	const std::size_t PayloadOff = kHeaderSize;
	const std::size_t PayloadSize = MmapCopy_.size() - PayloadOff;
	if((Catalog_.Flags & AshdLayoutFlags::ChunkIndex) != AshdLayoutFlags::None && PayloadSize >= 4) {
		const auto *Base = MmapCopy_.data() + PayloadOff;
		const uint32_t Count = static_cast<uint32_t>(Base[0]) | (static_cast<uint32_t>(Base[1]) << 8) |
		                       (static_cast<uint32_t>(Base[2]) << 16) | (static_cast<uint32_t>(Base[3]) << 24);
		std::size_t Off = 4;
		for(uint32_t I = 0; I < Count && Off + 16 <= PayloadSize; ++I) {
			AshdChunkIndexEntry E;
			const uint8_t NameLen = static_cast<uint8_t>(Base[Off++]);
			if(Off + NameLen + 15 > PayloadSize)
				break;
			E.Column.assign(reinterpret_cast<const char *>(Base + Off), NameLen);
			Off += NameLen;
			E.PayloadOffset = static_cast<std::size_t>(Base[Off]) | (static_cast<std::size_t>(Base[Off + 1]) << 8) |
			                  (static_cast<std::size_t>(Base[Off + 2]) << 16) |
			                  (static_cast<std::size_t>(Base[Off + 3]) << 24);
			Off += 4;
			E.PayloadSize = static_cast<std::size_t>(Base[Off]) | (static_cast<std::size_t>(Base[Off + 1]) << 8) |
			                (static_cast<std::size_t>(Base[Off + 2]) << 16) |
			                (static_cast<std::size_t>(Base[Off + 3]) << 24);
			Off += 4;
			E.Encoding = static_cast<ColumnEncoding>(Base[Off++]);
			E.RowCount = static_cast<std::size_t>(Base[Off]) | (static_cast<std::size_t>(Base[Off + 1]) << 8) |
			             (static_cast<std::size_t>(Base[Off + 2]) << 16) |
			             (static_cast<std::size_t>(Base[Off + 3]) << 24);
			Off += 4;
			Catalog_.Chunks.push_back(std::move(E));
			if(E.RowCount > RowCount_)
				RowCount_ = E.RowCount;
		}
	}
	if((Catalog_.Flags & AshdLayoutFlags::PartialAggFooter) != AshdLayoutFlags::None) {
		const std::size_t PayloadEnd = PayloadOff + PayloadSize;
		const std::size_t ChunkEnd = FooterScanStart(Catalog_, PayloadOff, PayloadSize);
		if(ChunkEnd < PayloadEnd) {
			const std::size_t FooterSize = PayloadEnd - ChunkEnd;
			ParsePartialAggFooter(MmapCopy_.data() + ChunkEnd, FooterSize, Catalog_.PartialAggFooter);
		}
	}
	Valid_ = true;
}

ColumnView LsmBypassReader::OpenColumn(std::string_view Column) const {
	for(const auto &E : Catalog_.Chunks) {
		if(E.Column != Column)
			continue;
		const std::size_t Abs = kHeaderSize + E.PayloadOffset;
		if(Abs + E.PayloadSize > MmapCopy_.size())
			return {};
		const std::byte *Data = MmapCopy_.data() + Abs;
		if((Catalog_.Flags & AshdLayoutFlags::TypedChunks) != AshdLayoutFlags::None &&
		   Catalog_.Compression != ColumnShardCompression::Off) {
			ColumnChunk Dec = ChunkCompressor::Decompress(Data, E.PayloadSize, Catalog_.Compression);
			return ColumnView(Dec.Payload.data(), Dec.Payload.size(), E.Encoding, E.RowCount);
		}
		return ColumnView(Data, E.PayloadSize, E.Encoding, E.RowCount);
	}
	return {};
}

} // namespace AstralDB
