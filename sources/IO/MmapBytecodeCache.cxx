#include <IO/MmapBytecodeCache.hxx>

#include <DS/Blake3.hxx>
#include <SQL/Bytecode/BytecodeFormat.hxx>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace AstralDB {
namespace SQL {

namespace {

struct MmapView {
	const std::byte *Data = nullptr;
	std::size_t Size = 0;
#if defined(__linux__)
	int Fd = -1;
#endif
#if defined(_WIN32)
	HANDLE File = INVALID_HANDLE_VALUE;
	HANDLE Mapping = nullptr;
#endif

	~MmapView() { Unmap(); }

	void Unmap() {
#if defined(__linux__)
		if(Data && Size)
			::munmap(const_cast<std::byte *>(Data), Size);
		if(Fd >= 0)
			::close(Fd);
		Fd = -1;
#endif
#if defined(_WIN32)
		if(Data)
			UnmapViewOfFile(Data);
		if(Mapping)
			CloseHandle(Mapping);
		if(File != INVALID_HANDLE_VALUE)
			CloseHandle(File);
		Mapping = nullptr;
		File = INVALID_HANDLE_VALUE;
#endif
		Data = nullptr;
		Size = 0;
	}

	bool MapFile(const std::filesystem::path &Path) {
		Unmap();
#if defined(__linux__)
		Fd = ::open(Path.string().c_str(), O_RDONLY);
		if(Fd < 0)
			return false;
		struct stat St {};
		if(::fstat(Fd, &St) != 0 || St.st_size <= 0)
			return false;
		Size = static_cast<std::size_t>(St.st_size);
		void *P = ::mmap(nullptr, Size, PROT_READ, MAP_PRIVATE, Fd, 0);
		if(P == MAP_FAILED) {
			Size = 0;
			return false;
		}
		Data = static_cast<const std::byte *>(P);
		::madvise(P, Size, MADV_SEQUENTIAL);
		return true;
#endif
#if defined(_WIN32)
		File = CreateFileW(Path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                   FILE_ATTRIBUTE_NORMAL, nullptr);
		if(File == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER Sz{};
		if(!GetFileSizeEx(File, &Sz) || Sz.QuadPart <= 0)
			return false;
		Size = static_cast<std::size_t>(Sz.QuadPart);
		Mapping = CreateFileMappingW(File, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if(!Mapping)
			return false;
		Data = static_cast<const std::byte *>(MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, 0));
		return Data != nullptr;
#endif
		return false;
	}
};

Instruction ReadInstructionFromView(const std::byte *&Cursor, const std::byte *End) {
	if(Cursor + 1 + 4 > End)
		throw std::runtime_error("Truncated bytecode instruction stream.");
	const std::uint8_t OpcodeByte = static_cast<std::uint8_t>(*Cursor++);
	std::uint32_t NumOperands = 0;
	std::memcpy(&NumOperands, Cursor, 4);
	Cursor += 4;
	Instruction Inst;
	Inst.Opcode_ = static_cast<Opcode>(OpcodeByte);
	Inst.Operands.reserve(NumOperands);
	for(std::uint32_t J = 0; J < NumOperands; ++J) {
		if(Cursor + 1 > End)
			throw std::runtime_error("Truncated bytecode operand stream.");
		const std::uint8_t Type = static_cast<std::uint8_t>(*Cursor++);
		if(Type == 0) {
			if(Cursor + 8 > End)
				throw std::runtime_error("Truncated int64 operand.");
			int64_t Value = 0;
			std::memcpy(&Value, Cursor, 8);
			Cursor += 8;
			Inst.Operands.push_back(Value);
		} else if(Type == 1) {
			if(Cursor + 4 > End)
				throw std::runtime_error("Truncated string operand length.");
			std::uint32_t Length = 0;
			std::memcpy(&Length, Cursor, 4);
			Cursor += 4;
			if(Cursor + Length > End)
				throw std::runtime_error("Truncated string operand.");
			std::string Value(reinterpret_cast<const char *>(Cursor), Length);
			Cursor += Length;
			Inst.Operands.push_back(std::move(Value));
		} else {
			throw std::runtime_error("Unknown operand type in mapped bytecode.");
		}
	}
	return Inst;
}

LoadedAbcFile ParseMappedAbc(const std::byte *Data, std::size_t Size) {
	if(Size < 8)
		throw std::runtime_error("Mapped bytecode file too small.");
	LoadedAbcFile Out;
	std::memcpy(&Out.ContainerVersion, Data, 4);
	if(Out.ContainerVersion != kAbcContainerVersion)
		throw std::runtime_error("Unsupported bytecode container version in mmap cache.");
	std::uint32_t NumInstructions = 0;
	std::memcpy(&NumInstructions, Data + 4, 4);
	const std::byte *Cursor = Data + 8;
	const std::byte *End = Data + Size;
	Out.Instructions.reserve(NumInstructions);
	for(std::uint32_t I = 0; I < NumInstructions; ++I)
		Out.Instructions.push_back(ReadInstructionFromView(Cursor, End));
	return Out;
}

std::string HashSqlKey(const std::string &Sql) {
	const std::vector<uint8_t> Input(Sql.begin(), Sql.end());
	const auto Digest = Blake3::Hash(Input);
	std::ostringstream O;
	for(std::size_t I = 0; I < Digest.size(); ++I)
		O << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(Digest[I]);
	return O.str();
}

} // namespace

LoadedAbcFile LoadAbcMapped(const std::filesystem::path &Path) {
	MmapView View;
	if(!View.MapFile(Path))
		throw std::runtime_error("Cannot mmap bytecode cache file: " + Path.string());
	return ParseMappedAbc(View.Data, View.Size);
}

MmapBytecodeCache::MmapBytecodeCache(std::filesystem::path CacheDir) : CacheDir_(std::move(CacheDir)) {
	std::error_code Ec;
	std::filesystem::create_directories(CacheDir_, Ec);
}

std::filesystem::path MmapBytecodeCache::PathForSql(const std::string &Sql) const {
	return CacheDir_ / (HashSqlKey(Sql) + ".abc");
}

LoadedAbcFile MmapBytecodeCache::LoadMapped(const std::filesystem::path &Path) const {
	return LoadAbcMapped(Path);
}

LoadedAbcFile MmapBytecodeCache::LoadOrCompile(const std::string &Sql,
                                               const std::function<CompiledBytecode()> &CompileFn) {
	LastHit_ = false;
	const std::filesystem::path Path = PathForSql(Sql);
	if(std::filesystem::exists(Path)) {
		LastHit_ = true;
		return LoadMapped(Path);
	}
	const CompiledBytecode Compiled = CompileFn();
	SaveAbcFile(Path, Compiled);
	LastHit_ = false;
	return LoadMapped(Path);
}

void MmapBytecodeCache::Invalidate(const std::string &Sql) {
	std::error_code Ec;
	std::filesystem::remove(PathForSql(Sql), Ec);
}

} // namespace SQL
} // namespace AstralDB
