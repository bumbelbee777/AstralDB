#include <DS/ShardCompress.hxx>
#include <DS/LZ4.hxx>

#if defined(_WIN32) && defined(_MSC_VER)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <compressapi.h>
#pragma comment(lib, "cabinet.lib")
#endif

namespace AstralDB {
namespace DS {
namespace {

constexpr char kMagic[4] = {'A', 'S', 'D', 'B'};

std::string PackFrame(ColumnShardCompression Mode, std::string Payload) {
	std::string Out;
	Out.reserve(8 + Payload.size());
	Out.append(kMagic, 4);
	Out.push_back(static_cast<char>(Mode));
	Out.push_back('\0');
	Out.push_back('\0');
	Out.push_back('\0');
	Out.append(Payload);
	return Out;
}

bool UnpackFrame(std::string_view Blob, ColumnShardCompression &Mode, std::string_view &Payload) {
	if(Blob.size() < 8 || Blob[0] != 'A' || Blob[1] != 'S' || Blob[2] != 'D' || Blob[3] != 'B')
		return false;
	Mode = static_cast<ColumnShardCompression>(static_cast<unsigned char>(Blob[4]));
	Payload = Blob.substr(8);
	return true;
}

#if defined(_WIN32) && defined(_MSC_VER)
std::string WinCompress(std::string_view Plain, COMPRESS_ALGORITHM Algorithm) {
	if(Plain.empty())
		return PackFrame(ColumnShardCompression::Lzx, {});
	COMPRESSOR_HANDLE Handle = nullptr;
	if(!CreateCompressor(Algorithm, nullptr, &Handle))
		return {};
	SIZE_T Bound = 0;
	if(!Compress(Handle, Plain.data(), Plain.size(), nullptr, 0, &Bound)) {
		CloseCompressor(Handle);
		return {};
	}
	std::string Out;
	Out.resize(Bound);
	SIZE_T Written = 0;
	const BOOL Ok = Compress(Handle, Plain.data(), Plain.size(), Out.data(), Out.size(), &Written);
	CloseCompressor(Handle);
	if(!Ok)
		return {};
	Out.resize(Written);
	return PackFrame(ColumnShardCompression::Lzx, Out);
}

std::string WinDecompress(std::string_view Blob, COMPRESS_ALGORITHM Algorithm) {
	if(Blob.empty())
		return {};
	DECOMPRESSOR_HANDLE Handle = nullptr;
	if(!CreateDecompressor(Algorithm, nullptr, &Handle))
		return {};
	SIZE_T Bound = Blob.size() * 4 + 65536;
	std::string Out;
	for(int Attempt = 0; Attempt < 8; ++Attempt) {
		Out.assign(Bound, '\0');
		SIZE_T Written = 0;
		if(Decompress(Handle, Blob.data(), Blob.size(), Out.data(), Out.size(), &Written)) {
			Out.resize(Written);
			CloseDecompressor(Handle);
			return Out;
		}
		Bound *= 2;
	}
	CloseDecompressor(Handle);
	return {};
}
#endif

} // namespace

std::string ShardCompress(std::string_view Plain, ColumnShardCompression Mode) {
	switch(Mode) {
	case ColumnShardCompression::Off:
		return PackFrame(ColumnShardCompression::Off, std::string(Plain));
	case ColumnShardCompression::Lzx:
#if defined(_WIN32) && defined(_MSC_VER)
		if(auto Lzx = WinCompress(Plain, COMPRESS_ALGORITHM_LZX))
			return Lzx;
#endif
		[[fallthrough]];
	case ColumnShardCompression::Lz4:
	default: {
		const std::string C = LZ4Compress(std::string(Plain));
		return PackFrame(ColumnShardCompression::Lz4, C);
	}
	}
}

std::string ShardDecompress(std::string_view Blob, ColumnShardCompression ModeHint) {
	ColumnShardCompression Mode = ModeHint;
	std::string_view Payload;
	if(!UnpackFrame(Blob, Mode, Payload))
		return std::string(Blob);
	if(Mode == ColumnShardCompression::Off)
		return std::string(Payload);
	if(Mode == ColumnShardCompression::Lz4)
		return LZ4Decompress(std::string(Payload));
#if defined(_WIN32) && defined(_MSC_VER)
	return WinDecompress(Payload, COMPRESS_ALGORITHM_LZX);
#else
	return LZ4Decompress(std::string(Payload));
#endif
}

} // namespace DS
} // namespace AstralDB
