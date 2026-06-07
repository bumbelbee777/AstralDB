#include <IO/ColumnShardStream.hxx>

#include <DS/ShardCompress.hxx>
#include <IO/ColumnShardRegistry.hxx>
#include <IO/Job.hxx>
#include <IO/MemoryGuard.hxx>

#include <algorithm>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AstralDB {

namespace {

constexpr std::size_t kShardBufferBytes = 1u << 20;
constexpr char kShardMagic[4] = {'A', 'S', 'H', 'D'};

std::filesystem::path ColumnPath(const std::filesystem::path &Root, const std::string &Table,
                                 const std::string &Column) {
	return Root / (Table + "__" + Column + ".shard");
}

std::string ReadFileBytes(const std::filesystem::path &Path) {
	std::ifstream In(Path, std::ios::binary);
	if(!In)
		return {};
	In.seekg(0, std::ios::end);
	const auto Sz = In.tellg();
	if(Sz <= 0)
		return {};
	std::string Buf(static_cast<std::size_t>(Sz), '\0');
	In.seekg(0, std::ios::beg);
	In.read(Buf.data(), Sz);
	return Buf;
}

void WriteFileBytes(const std::filesystem::path &Path, std::string_view Data) {
	std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
	if(!Out)
		throw std::runtime_error("ColumnShardStream: cannot write " + Path.string());
	Out.write(Data.data(), static_cast<std::streamsize>(Data.size()));
}

void BuildOffsetsFromPlain(std::string_view Plain, std::vector<std::size_t> &Offsets) {
	Offsets.clear();
	Offsets.push_back(0);
	std::size_t Pos = 0;
	while(Pos < Plain.size()) {
		const std::size_t Nl = Plain.find('\n', Pos);
		if(Nl == std::string_view::npos)
			break;
		Pos = Nl + 1;
		Offsets.push_back(Pos);
	}
}

std::string DecodeShardPayload(std::string_view FileBytes, ColumnShardCompression /*ModeHint*/, std::string &PlainOut) {
	if(FileBytes.size() >= 8 && FileBytes[0] == kShardMagic[0] && FileBytes[1] == kShardMagic[1] &&
	   FileBytes[2] == kShardMagic[2] && FileBytes[3] == kShardMagic[3]) {
		const auto Mode = static_cast<ColumnShardCompression>(static_cast<unsigned char>(FileBytes[4]));
		const std::string_view Payload = FileBytes.substr(8);
		if(Mode == ColumnShardCompression::Off) {
			PlainOut.assign(Payload);
			return PlainOut;
		}
		PlainOut = DS::ShardDecompress(Payload, Mode);
		return PlainOut;
	}
	PlainOut.assign(FileBytes);
	return PlainOut;
}

#if defined(_WIN32)
void PrefetchShardFilesIocp(const std::vector<std::filesystem::path> &Paths, unsigned Depth) {
	if(Paths.empty() || Depth == 0)
		return;
	const unsigned Workers = (std::min)(Depth, static_cast<unsigned>(Paths.size()));
	std::vector<std::future<void>> Jobs;
	Jobs.reserve(Paths.size());
	for(const auto &P : Paths) {
		Jobs.push_back(JobSystem::Instance().SubmitAsync([P]() {
			HANDLE File = CreateFileW(P.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if(File == INVALID_HANDLE_VALUE)
				return;
			const DWORD Chunk = 1u << 16;
			std::vector<char> Buf(Chunk);
			DWORD Read = 0;
			while(ReadFile(File, Buf.data(), Chunk, &Read, nullptr) && Read > 0) {
			}
			CloseHandle(File);
		}));
		if(Jobs.size() >= Workers) {
			for(auto &J : Jobs)
				J.get();
			Jobs.clear();
		}
	}
	for(auto &J : Jobs)
		J.get();
}
#endif

} // namespace

class BufferedFileShard {
	std::filesystem::path Path_;
	std::string PlainBuffer_;
	std::vector<std::size_t> Offsets_;
	ColumnShardCompression Compression_ = ColumnShardCompression::Lz4;
	bool ReadOnly_ = false;
#if defined(__linux__)
	int Fd_ = -1;
	char *MmapPtr_ = nullptr;
	std::size_t MappedSize_ = 0;
#endif
#if defined(_WIN32)
	HANDLE FileHandle_ = INVALID_HANDLE_VALUE;
	HANDLE MapHandle_ = nullptr;
	const char *ViewPtr_ = nullptr;
	std::size_t ViewSize_ = 0;
#endif

public:
	explicit BufferedFileShard(std::filesystem::path Path, ColumnShardCompression Compression, bool ReadOnly = false)
	    : Path_(std::move(Path)), Compression_(Compression), ReadOnly_(ReadOnly) {
		if(ReadOnly) {
			LoadFromDisk();
			return;
		}
		Offsets_.push_back(0);
	}

	~BufferedFileShard() {
#if defined(__linux__)
		if(MmapPtr_ != nullptr)
			::munmap(MmapPtr_, MappedSize_);
		if(Fd_ >= 0)
			::close(Fd_);
#endif
#if defined(_WIN32)
		if(ViewPtr_ != nullptr)
			UnmapViewOfFile(ViewPtr_);
		if(MapHandle_ != nullptr)
			CloseHandle(MapHandle_);
		if(FileHandle_ != INVALID_HANDLE_VALUE)
			CloseHandle(FileHandle_);
#endif
	}

	void LoadFromDisk() {
		const std::string FileBytes = ReadFileBytes(Path_);
		std::string Plain;
		DecodeShardPayload(FileBytes, Compression_, Plain);
		PlainBuffer_ = std::move(Plain);
		BuildOffsetsFromPlain(PlainBuffer_, Offsets_);
		MapForRead();
	}

	void MapForRead() {
#if defined(__linux__)
		if(!PlainBuffer_.empty() || !std::filesystem::exists(Path_))
			return;
		Fd_ = ::open(Path_.string().c_str(), O_RDONLY);
		if(Fd_ < 0)
			return;
		struct stat St {};
		if(::fstat(Fd_, &St) != 0 || St.st_size <= 0)
			return;
		MappedSize_ = static_cast<std::size_t>(St.st_size);
		void *P = ::mmap(nullptr, MappedSize_, PROT_READ, MAP_PRIVATE, Fd_, 0);
		if(P == MAP_FAILED) {
			MappedSize_ = 0;
			return;
		}
		MmapPtr_ = static_cast<char *>(P);
#endif
#if defined(_WIN32)
		if(!PlainBuffer_.empty())
			return;
		FileHandle_ = CreateFileW(Path_.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                          FILE_ATTRIBUTE_NORMAL, nullptr);
		if(FileHandle_ == INVALID_HANDLE_VALUE)
			return;
		LARGE_INTEGER Sz{};
		if(!GetFileSizeEx(FileHandle_, &Sz) || Sz.QuadPart <= 0)
			return;
		ViewSize_ = static_cast<std::size_t>(Sz.QuadPart);
		MapHandle_ = CreateFileMappingW(FileHandle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if(!MapHandle_)
			return;
		ViewPtr_ = static_cast<const char *>(MapViewOfFile(MapHandle_, FILE_MAP_READ, 0, 0, 0));
		if(ViewPtr_) {
			std::string Plain;
			DecodeShardPayload(std::string_view(ViewPtr_, ViewSize_), Compression_, Plain);
			PlainBuffer_ = std::move(Plain);
			BuildOffsetsFromPlain(PlainBuffer_, Offsets_);
		}
#endif
	}

	void Append(std::string_view Value) {
		PlainBuffer_.append(Value);
		PlainBuffer_.push_back('\n');
		Offsets_.push_back(PlainBuffer_.size());
	}

	void CloseWrite() {
		if(ReadOnly_)
			return;
		std::string Payload;
		if(Compression_ == ColumnShardCompression::Off) {
			Payload = PlainBuffer_;
		} else {
			Payload = DS::ShardCompress(PlainBuffer_, Compression_);
		}
		std::string FileBody;
		FileBody.reserve(8 + Payload.size());
		FileBody.append(kShardMagic, 4);
		FileBody.push_back(static_cast<char>(Compression_));
		FileBody.append(3, '\0');
		FileBody.append(Payload);
		WriteFileBytes(Path_, FileBody);
		PlainBuffer_.clear();
		PlainBuffer_.shrink_to_fit();
		MapForRead();
	}

	[[nodiscard]] std::size_t RowCount() const noexcept {
		return Offsets_.size() > 0 ? Offsets_.size() - 1 : 0;
	}

	[[nodiscard]] std::string ReadAt(std::size_t Index) const {
		if(Index + 1 >= Offsets_.size())
			return {};
		const std::size_t Beg = Offsets_[Index];
		const std::size_t End = Offsets_[Index + 1];
		if(End <= Beg || End > PlainBuffer_.size() + 1)
			return {};
		if(End == Beg + 1)
			return {};
		return PlainBuffer_.substr(Beg, End - Beg - 1);
	}

	[[nodiscard]] const std::filesystem::path &Path() const noexcept { return Path_; }
};

struct ColumnShardWriter::Impl {
	std::filesystem::path Root;
	ColumnShardConfig Config;
	std::string Table;
	std::vector<std::string> Columns;
	std::vector<std::unique_ptr<BufferedFileShard>> Shards;
	std::size_t RowsWritten = 0;
};

struct ColumnShardReader::Impl {
	ColumnShardConfig Config;
	std::vector<std::string> Columns;
	std::vector<std::unique_ptr<BufferedFileShard>> Shards;
	std::size_t RowCount = 0;
};

ColumnShardStreamCapabilities ProbeColumnShardStreamCapabilities() noexcept {
	ColumnShardStreamCapabilities Cap;
#if defined(__linux__)
	Cap.MmapRead = true;
#if defined(ASTRALDB_HAVE_IO_URING)
	Cap.IoUringAsync = true;
#endif
#endif
#if defined(_WIN32)
	Cap.IocpAsync = true;
#endif
	return Cap;
}

ColumnShardWriter::ColumnShardWriter(std::filesystem::path RootDir, ColumnShardConfig Config)
    : Impl_(std::make_unique<Impl>()) {
	ColumnShardRegistry::PurgeExpired();
	Impl_->Config = Config;
	Impl_->Root = std::move(RootDir);
	std::filesystem::create_directories(Impl_->Root);
	ColumnShardRegistry::RegisterDirectory(Impl_->Root, Impl_->Config.TtlSeconds);
}

ColumnShardWriter::~ColumnShardWriter() {
	if(Impl_ && !Impl_->Table.empty())
		FinalizeTable();
}

void ColumnShardWriter::BeginTable(const std::string &TableName, const std::vector<std::string> &Columns) {
	FinalizeTable();
	Impl_->Table = TableName;
	Impl_->Columns = Columns;
	Impl_->Shards.clear();
	Impl_->Shards.reserve(Columns.size());
	for(const auto &Col : Columns)
		Impl_->Shards.push_back(
		    std::make_unique<BufferedFileShard>(ColumnPath(Impl_->Root, TableName, Col), Impl_->Config.Compression));
	Impl_->RowsWritten = 0;
}

void ColumnShardWriter::AppendRow(int64_t RowId, int64_t Step, const std::vector<std::string> &ColNames,
                                  const std::vector<std::string> &Values) {
	(void)RowId;
	(void)Step;
	if(Impl_->Shards.size() != Values.size() || ColNames.size() != Values.size())
		throw std::runtime_error("ColumnShardWriter: column/value mismatch");
	for(std::size_t I = 0; I < Impl_->Shards.size(); ++I)
		Impl_->Shards[I]->Append(Values[I]);
	++Impl_->RowsWritten;
	if((Impl_->RowsWritten & 0xFFFFu) == 0)
		MemoryGuard::NoteAlloc(kShardBufferBytes);
}

void ColumnShardWriter::FinalizeTable() {
	for(auto &S : Impl_->Shards)
		S->CloseWrite();
	Impl_->Shards.clear();
	Impl_->Table.clear();
	Impl_->Columns.clear();
}

std::size_t ColumnShardWriter::RowsWritten() const noexcept { return Impl_->RowsWritten; }

const std::filesystem::path &ColumnShardWriter::Root() const noexcept { return Impl_->Root; }

const ColumnShardConfig &ColumnShardWriter::Config() const noexcept { return Impl_->Config; }

ColumnShardReader::ColumnShardReader(std::filesystem::path ShardDir, ColumnShardConfig Config)
    : Impl_(std::make_unique<Impl>()) {
	Impl_->Config = Config;
	if(!std::filesystem::is_directory(ShardDir))
		return;
	std::string TablePrefix;
	std::vector<std::filesystem::path> Paths;
	for(const auto &Ent : std::filesystem::directory_iterator(ShardDir)) {
		if(!Ent.is_regular_file())
			continue;
		const auto Name = Ent.path().filename().string();
		const auto Pos = Name.find("__");
		const auto Dot = Name.find(".shard");
		if(Pos == std::string::npos || Dot == std::string::npos)
			continue;
		const std::string Tbl = Name.substr(0, Pos);
		const std::string Col = Name.substr(Pos + 2, Dot - (Pos + 2));
		if(TablePrefix.empty())
			TablePrefix = Tbl;
		if(Tbl != TablePrefix)
			continue;
		Impl_->Columns.push_back(Col);
		Paths.push_back(Ent.path());
		Impl_->Shards.push_back(
		    std::make_unique<BufferedFileShard>(Ent.path(), Config.Compression, true));
	}
#if defined(_WIN32)
	PrefetchShardFilesIocp(Paths, Config.IocpReadAheadChunks > 0 ? Config.IocpReadAheadChunks : Config.IocpDepth);
#endif
	std::vector<std::size_t> Order(Impl_->Columns.size());
	std::iota(Order.begin(), Order.end(), std::size_t{0});
	std::sort(Order.begin(), Order.end(),
	          [&](std::size_t A, std::size_t B) { return Impl_->Columns[A] < Impl_->Columns[B]; });
	std::vector<std::string> SortedCols;
	std::vector<std::unique_ptr<BufferedFileShard>> SortedShards;
	for(std::size_t I : Order) {
		SortedCols.push_back(Impl_->Columns[I]);
		SortedShards.push_back(std::move(Impl_->Shards[I]));
	}
	Impl_->Columns = std::move(SortedCols);
	Impl_->Shards = std::move(SortedShards);
	if(!Impl_->Shards.empty())
		Impl_->RowCount = Impl_->Shards[0]->RowCount();
}

std::size_t ColumnShardReader::RowCount() const noexcept { return Impl_->RowCount; }

const std::vector<std::string> &ColumnShardReader::ColumnNames() const noexcept { return Impl_->Columns; }

std::string ColumnShardReader::Cell(std::size_t RowIndex, const std::string &Column) const {
	for(std::size_t I = 0; I < Impl_->Columns.size(); ++I) {
		if(Impl_->Columns[I] == Column)
			return Impl_->Shards[I]->ReadAt(RowIndex);
	}
	return {};
}

} // namespace AstralDB
