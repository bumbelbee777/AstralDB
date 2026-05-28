#include <Database/AdvancedTypes.hxx>
#include <Database/Database.hxx>
#include <Database/GraphStorage.hxx>
#include <Database/MathSciComplex.hxx>
#include <Database/ColumnarStorage.hxx>
#include <Database/EmbeddingStorage.hxx>
#include <IO/ArenaAllocator.hxx>
#include <IO/MemoryGuard.hxx>
#include <cstring>
#include <Database/AtRestKey.hxx>
#include <SQL/SetExprEval.hxx>
#include <SQL/DialectCompat.hxx>
#include <IO/Error.hxx>
#include <DS/LZ4.hxx>
#include <DS/XChaCha20.hxx>
#include <optional>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <tuple>
#include <string>
#include <string_view>
#include <chrono>
#include <thread>
#include <system_error>
#include <atomic>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <array>
#include <cstring>
#include <limits>
#include <random>
#include <vector>
#include <string_view>
#include <cctype>
#include <DS/TabularData.hxx>
#include <DS/JSON.hxx>
#include <SQL/Bytecode.hxx>
#include <SQL/BytecodeProcedures.hxx>
#include <SQL/BytecodeTriggers.hxx>
#include <Database/TriggerRuntime.hxx>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AstralDB {

namespace {

thread_local int TlDbExclusiveDepth = 0;

std::vector<SQL::StoredTriggerEntry> TriggerRegistryVector(
    const std::unordered_map<std::string, SQL::StoredTriggerEntry> &Map) {
	std::vector<SQL::StoredTriggerEntry> Out;
	Out.reserve(Map.size());
	for(const auto &Pr : Map)
		Out.push_back(Pr.second);
	return Out;
}

} // namespace

bool DatabaseVmAssumeDbMutexHeld() noexcept { return TlDbExclusiveDepth > 0; }

void DatabaseVmBumpDbMutexDepth() noexcept { ++TlDbExclusiveDepth; }

void DatabaseVmPopDbMutexDepth() noexcept {
	if(TlDbExclusiveDepth > 0)
		--TlDbExclusiveDepth;
}

static constexpr std::string_view kTriggerSnapshotMarkerSv = "<<<ASTRAL_DB_TRIGGERS>>>\n";

static void AppendTriggerSnapshotTrailer(std::string &RawData,
                                         const std::unordered_map<std::string, SQL::StoredTriggerEntry> &Triggers) {
	if(Triggers.empty())
		return;
	RawData.append(kTriggerSnapshotMarkerSv.data(), kTriggerSnapshotMarkerSv.size());
	const auto Blob = SQL::SerializeTriggerRegistry(TriggerRegistryVector(Triggers));
	RawData += std::to_string(Blob.size());
	RawData.push_back('\n');
	RawData += Blob;
	RawData.push_back('\n');
}

static bool StripAndParseTriggerSnapshotTrailer(std::string &RawData,
                                              std::unordered_map<std::string, SQL::StoredTriggerEntry> &Out) {
	Out.clear();
	const size_t Mp = RawData.find(kTriggerSnapshotMarkerSv.data(), 0, kTriggerSnapshotMarkerSv.size());
	if(Mp == std::string::npos)
		return true;
	std::string_view Tail(RawData.data() + Mp + kTriggerSnapshotMarkerSv.size(),
	                     RawData.size() - Mp - kTriggerSnapshotMarkerSv.size());
	const size_t NL = Tail.find('\n');
	if(NL == std::string_view::npos)
		return false;
	std::uint64_t BN = 0;
	for(unsigned char Ch : Tail.substr(0, NL)) {
		if(Ch < '0' || Ch > '9')
			return false;
		BN = BN * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(NL + 1);
	if(Tail.size() < BN + 1 || Tail[BN] != '\n')
		return false;
	const std::string Blob(Tail.substr(0, BN));
	std::vector<SQL::StoredTriggerEntry> Entries;
	if(!SQL::DeserializeTriggerRegistry(Blob, Entries))
		return false;
	for(SQL::StoredTriggerEntry &E : Entries)
		Out.emplace(E.Name, std::move(E));
	RawData.erase(Mp);
	return true;
}

namespace {

std::string NormFmt(std::string_view V) {
	std::string O(V.begin(), V.end());
	for(char &C : O)
		C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
	return O;
}

constexpr std::string_view kTableMarkerPrefix = "<<<AstralDB:TABLE:";

std::string MakeTableMarker(const std::string &TableName) {
	return std::string(kTableMarkerPrefix) + TableName + ">>>";
}

std::optional<std::string> ParseTableMarkerLine(const std::string &Line) {
	if(Line.size() < kTableMarkerPrefix.size() + 3)
		return std::nullopt;
	if(std::string_view(Line).substr(0, kTableMarkerPrefix.size()) != kTableMarkerPrefix)
		return std::nullopt;
	if(Line.compare(Line.size() - 3, 3, ">>>") != 0)
		return std::nullopt;
	return Line.substr(kTableMarkerPrefix.size(), Line.size() - kTableMarkerPrefix.size() - 3);
}

DS::Table BuildTabularForExport(const Database::Schema &Sch, const Database::Table &RowStore) {
	DS::Table T;
	for(const auto &Col : Sch)
		T.Headers.push_back(Col.Name);
	for(const auto &Item : RowStore) {
		DS::Row R;
		for(const auto &Col : Sch) {
			auto It = Item.find(Col.Name);
			R.Fields.push_back(It != Item.end() ? It->second : std::string());
		}
		T.Rows.push_back(std::move(R));
	}
	return T;
}

Database::Schema SchemaFromHeaders(const std::vector<std::string> &Headers, const std::string &SqlType) {
	Database::Schema Sch;
	for(const auto &H : Headers) {
		Database::Column C;
		C.Name = H;
		C.DefaultValue = SqlType;
		Sch.push_back(std::move(C));
	}
	return Sch;
}

[[noreturn]] static void FailStorage(std::string Message) {
	throw std::runtime_error(Err::Prefixed("storage", std::move(Message)));
}

std::string ReadPathText(const std::filesystem::path &P) {
	std::ifstream In(P, std::ios::binary);
	if(!In)
		FailStorage("Cannot read file (check the path and permissions): " + P.string());
	return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
}

} // namespace

namespace {

std::string SealAtRestPayload(std::string_view Plaintext) {
	if(Plaintext.empty())
		return "";
	std::array<uint8_t, 24> Nonce{};
	std::random_device Rd;
	for(auto &B : Nonce)
		B = static_cast<uint8_t>(Rd());
	std::vector<uint8_t> In(Plaintext.begin(), Plaintext.end()), Out;
	XChaCha20 Cipher(kAtRestXChaChaKey, Nonce);
	Cipher.Encrypt(In, Out);
	std::string Blob;
	Blob.resize(24 + Out.size());
	std::memcpy(Blob.data(), Nonce.data(), Nonce.size());
	std::memcpy(Blob.data() + Nonce.size(), Out.data(), Out.size());
	return Blob;
}

std::string UnsealAtRestPayload(std::string_view Blob) {
	if(Blob.size() < 24)
		return {};
	std::array<uint8_t, 24> Nonce{};
	std::memcpy(Nonce.data(), Blob.data(), 24);
	std::vector<uint8_t> In(Blob.begin() + 24, Blob.end()), Out;
	XChaCha20 Cipher(kAtRestXChaChaKey, Nonce);
	Cipher.Decrypt(In, Out);
	return std::string(Out.begin(), Out.end());
}

template<typename Fn>
std::future<std::invoke_result_t<std::decay_t<Fn>>> DbDispatchAsync(const Database *Self, Fn &&Callable) {
	Self->AcquireAsyncBudget();
	using Ret = std::invoke_result_t<std::decay_t<Fn>>;
	try {
		return std::async(std::launch::async, [Self, Job = std::forward<Fn>(Callable)]() mutable -> Ret {
			struct ScopedAsyncBudget {
				const Database *Db;
				explicit ScopedAsyncBudget(const Database *D) : Db(D) {}
				~ScopedAsyncBudget() {
					Db->ReleaseAsyncBudget();
				}
			} _(Self);
			if constexpr(std::is_void_v<Ret>) {
				Job();
				return;
			} else {
				return Job();
			}
		});
	} catch(...) {
		Self->ReleaseAsyncBudget();
		throw;
	}
}

} // namespace

void Database::AcquireAsyncBudget() const {
	for(;;) {
		auto Observed = OutstandingAsyncJobs_.load(std::memory_order_relaxed);
		if(Observed >= Limits::MaxOutstandingAsyncJobs) {
			std::this_thread::sleep_for(std::chrono::microseconds(500));
			continue;
		}
		if(OutstandingAsyncJobs_.compare_exchange_weak(Observed, Observed + 1, std::memory_order_acq_rel,
		                                               std::memory_order_relaxed))
			return;
	}
}

void Database::ReleaseAsyncBudget() const {
	OutstandingAsyncJobs_.fetch_sub(1, std::memory_order_acq_rel);
}

void Database::JoinFlushWorkerBestEffort() noexcept {
	if(!FlushWorkerThread_.joinable())
		return;
#if defined(_WIN32)
	HANDLE Wh = reinterpret_cast<HANDLE>(FlushWorkerThread_.native_handle());
#else
	void *Wh = nullptr;
	(void)Wh;
#endif
	try {
		FlushWorkerThread_.join();
		return;
	} catch(const std::system_error &) {
#if defined(_WIN32)
		/** libc++/Win32 occasional join failure — wait on OS thread, then retry join (avoid detach; detach here was
		 *  flaky under repeated SAVEPOINT reload). */
		if(Wh != nullptr && Wh != INVALID_HANDLE_VALUE &&
		   WaitForSingleObject(Wh, INFINITE) == WAIT_OBJECT_0) {
			try {
				FlushWorkerThread_.join();
			} catch(const std::system_error &) {}
		}
#endif
	}
}

void Database::FlushWorker() noexcept {
	using namespace std::chrono_literals;
	for(;;) {
		if(StopFlushWorker_.load(std::memory_order_acquire))
			break;
		if(Dirty_.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(5ms);
			if(StopFlushWorker_.load(std::memory_order_acquire))
				break;
			{
				std::scoped_lock Guard(DbMutex_);
				if(StopFlushWorker_.load(std::memory_order_acquire))
					break;
				if(Dirty_.load(std::memory_order_acquire)) {
					try {
						SyncToFileUnlocked();
					} catch(...) {
						// In production, log the error
					}
					Dirty_.store(false, std::memory_order_release);
				}
			}
			continue;
		}
		std::this_thread::sleep_for(10ms);
	}
}

Database::Database(const std::filesystem::path &DbPath, Logger* Logger)
	: CurrentUser_(std::nullopt)
	, Wal_(DbPath)
	, Logger_(Logger)
	, Dirty_(false)
	, StopFlushWorker_(false)
	, DbPath_(DbPath) {
	/** Replay / first SyncToFile must finish before the flush worker can call SyncToFileUnlocked. */
	const bool WalHadPayload =
	    Wal_.Exists() && std::filesystem::file_size(Wal_.Path()) > 0;
	bool LoadedSnapshot = false;
	if(std::filesystem::exists(DbPath_)) {
		std::filesystem::path SnapshotPath = DbPath_;
		LoadedSnapshot = LoadSnapshotFromDiskPathSynchronously(std::move(SnapshotPath));
		if(!LoadedSnapshot && Logger_)
			Logger_->Warn("Could not load existing database snapshot; attempting WAL replay only");
	}
	if(WalHadPayload) {
		WalSuspended_.store(true, std::memory_order_release);
		Wal_.Replay(*this);
		WalSuspended_.store(false, std::memory_order_release);
		if(!LoadedSnapshot)
			SyncToFile();
	}
	if(Users_.empty())
		Users_.emplace_back("Admin0", "admin", Permissions::All);
	{
		std::scoped_lock Guard(DbMutex_);
		EnsureBootstrapAdminAclAssumeLocked();
	}
	FlushWorkerThread_ = std::thread([this]() { this->FlushWorker(); });
	if(Logger_) Logger_->Info("Database initialized at " + DbPath.string());
}

namespace {

static const char *const kWalB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string WalEncodeSqlBody(std::string_view Plain) {
	std::string Out;
	Out.reserve(((Plain.size() + 2) / 3) * 4);
	uint32_t Acc = 0;
	int Bits = 0;
	for(unsigned char Ch : Plain) {
		Acc = (Acc << 8) | Ch;
		Bits += 8;
		while(Bits >= 6) {
			Bits -= 6;
			Out.push_back(kWalB64[(Acc >> Bits) & 63]);
		}
	}
	if(Bits) {
		Acc <<= (6 - Bits);
		Out.push_back(kWalB64[Acc & 63]);
	}
	while(Out.size() % 4)
		Out.push_back('=');
	return Out;
}

/** Same alphabet as WAL \c WalDecodeSqlBody (CREATE VIEW); decodes CREATE TABLE snippet/DNF payloads. */
static std::string WalDecodeSqlBodyB64(std::string_view In) {
	std::string Out;
	Out.reserve(In.size() * 3 / 4);
	int Acc = 0;
	int Bits = -8;
	for(unsigned char C : In) {
		if(C == '=')
			break;
		int D = -1;
		if(C >= 'A' && C <= 'Z')
			D = static_cast<int>(C - 'A');
		else if(C >= 'a' && C <= 'z')
			D = static_cast<int>(C - 'a') + 26;
		else if(C >= '0' && C <= '9')
			D = static_cast<int>(C - '0') + 52;
		else if(C == '+')
			D = 62;
		else if(C == '/')
			D = 63;
		if(D < 0)
			continue;
		Acc = (Acc << 6) | D;
		Bits += 6;
		if(Bits >= 0) {
			Out.push_back(static_cast<char>((Acc >> Bits) & 255));
			Bits -= 8;
		}
	}
	return Out;
}

static std::string WalEncodeOptionalConstraintB64(const std::optional<std::string> &Opt) {
	if(!Opt.has_value())
		return std::string("-");
	return WalEncodeSqlBody(*Opt);
}

static void ApplyWalDecodedConstraintTok(Database::Column &Co, bool IsSql, std::string_view Tok) {
	if(Tok.empty() || Tok == "-")
		return;
	if(IsSql)
		Co.CheckConstraintSql = WalDecodeSqlBodyB64(Tok);
	else
		Co.CheckConstraintDnfPacked = WalDecodeSqlBodyB64(Tok);
}

static void TryReadTrailingConstraintColumns(std::istringstream &In, Database::Column &Co) {
	In >> std::ws;
	while(In.peek() == '\r')
		In.get();
	const int P = In.peek();
	if(P == EOF || P == '\n')
		return;
	std::string EncSql;
	std::string EncDnf;
	if(!(In >> EncSql >> EncDnf))
		return;
	ApplyWalDecodedConstraintTok(Co, true, EncSql);
	ApplyWalDecodedConstraintTok(Co, false, EncDnf);
	std::string IdAlwaysTok;
	std::string IdSeqTok;
	if(!(In >> IdAlwaysTok >> IdSeqTok))
		return;
	if(IdAlwaysTok == "1" || IdAlwaysTok == "0") {
		Co.IsIdentity = true;
		Co.IdentityAlways = IdAlwaysTok == "1";
		if(IdSeqTok != "-")
			Co.IdentitySequenceName = IdSeqTok;
	}
}

static constexpr std::string_view kStorageSnapshotMarkerSv = "<<<ASTRAL_DB_STORAGE>>>\n";

static void AppendStorageSnapshotTrailer(std::string &RawData,
                                         const std::unordered_map<std::string, HybridTableSlot> &Tables) {
	std::size_t Count = 0;
	for(const auto &Pr : Tables) {
		if(Pr.second.DeclaredPolicy != StorageLayout::Row || Pr.second.Workload.ReadCount > 0 ||
		   Pr.second.Workload.WriteCount > 0)
			++Count;
	}
	if(Count == 0)
		return;
	RawData.append(kStorageSnapshotMarkerSv.data(), kStorageSnapshotMarkerSv.size());
	RawData += std::to_string(Count);
	RawData.push_back('\n');
	for(const auto &Pr : Tables) {
		const HybridTableSlot &Slot = Pr.second;
		if(Slot.DeclaredPolicy == StorageLayout::Row && Slot.Workload.ReadCount == 0 &&
		   Slot.Workload.WriteCount == 0)
			continue;
		const std::string Weights = Slot.Scheduler.SerializeWeights();
		RawData += std::to_string(Pr.first.size());
		RawData.push_back(' ');
		RawData += std::to_string(static_cast<unsigned>(Slot.DeclaredPolicy));
		RawData.push_back(' ');
		RawData += std::to_string(Weights.size());
		RawData.push_back('\n');
		RawData += Pr.first;
		RawData.push_back('\n');
		RawData += Weights;
		RawData.push_back('\n');
	}
}

struct StorageSnapshotEntry {
	StorageLayout Policy = StorageLayout::Row;
	std::string Weights;
};

static bool StripAndParseStorageSnapshotTrailer(std::string &RawData,
                                                std::unordered_map<std::string, StorageSnapshotEntry> &Out) {
	Out.clear();
	const size_t Mp = RawData.find(kStorageSnapshotMarkerSv.data(), 0, kStorageSnapshotMarkerSv.size());
	if(Mp == std::string::npos)
		return true;
	std::string_view Tail(RawData.data() + Mp + kStorageSnapshotMarkerSv.size(),
	                      RawData.size() - Mp - kStorageSnapshotMarkerSv.size());
	const size_t Nl = Tail.find('\n');
	if(Nl == std::string_view::npos)
		return false;
	std::uint64_t NT = 0;
	for(unsigned char Ch : Tail.substr(0, Nl)) {
		if(Ch < '0' || Ch > '9')
			return false;
		NT = NT * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(Nl + 1);
	for(std::uint64_t I = 0; I < NT; ++I) {
		const size_t Ln = Tail.find('\n');
		if(Ln == std::string_view::npos)
			return false;
		const std::string_view LenLine = Tail.substr(0, Ln);
		Tail = Tail.substr(Ln + 1);
		std::istringstream MetaParseStream{std::string(LenLine)};
		std::uint64_t NameLen = 0, PolicyVal = 0, WeightsLen = 0;
		if(!(MetaParseStream >> NameLen >> PolicyVal >> WeightsLen))
			return false;
		if(Tail.size() < NameLen + 1 + WeightsLen + 1)
			return false;
		std::string TName(Tail.substr(0, NameLen));
		if(Tail[NameLen] != '\n')
			return false;
		Tail = Tail.substr(NameLen + 1);
		StorageSnapshotEntry Entry;
		Entry.Policy = static_cast<StorageLayout>(PolicyVal);
		Entry.Weights.assign(Tail.substr(0, WeightsLen));
		if(Tail[WeightsLen] != '\n')
			return false;
		Tail = Tail.substr(WeightsLen + 1);
		Out[std::move(TName)] = std::move(Entry);
	}
	RawData.erase(Mp);
	return true;
}

static constexpr std::string_view kViewSnapshotMarkerSv = "<<<ASTRAL_DB_VIEWS>>>\n";

static void AppendViewSnapshotTrailer(std::string &RawData,
                                      const std::unordered_map<std::string, std::string> &Views) {
	if(Views.empty())
		return;
	RawData.append(kViewSnapshotMarkerSv.data(), kViewSnapshotMarkerSv.size());
	RawData += std::to_string(Views.size());
	RawData.push_back('\n');
	for(const auto &Pr : Views) {
		RawData += std::to_string(Pr.first.size());
		RawData.push_back(' ');
		RawData += std::to_string(Pr.second.size());
		RawData.push_back('\n');
		RawData += Pr.first;
		RawData.push_back('\n');
		RawData += Pr.second;
		RawData.push_back('\n');
	}
}

/** Remove optional view snapshot trailer from RawData (so legacy numeric istream parsing still works) and decode it. */
static bool StripAndParseViewSnapshotTrailer(std::string &RawData,
                                            std::unordered_map<std::string, std::string> &OutViews) {
	OutViews.clear();
	const size_t Mp = RawData.find(kViewSnapshotMarkerSv.data(), 0, kViewSnapshotMarkerSv.size());
	if(Mp == std::string::npos)
		return true;
	std::string_view Tail(RawData.data() + Mp + kViewSnapshotMarkerSv.size(),
	                     RawData.size() - Mp - kViewSnapshotMarkerSv.size());
	std::size_t NewlineAfterCount = Tail.find('\n');
	if(NewlineAfterCount == std::string_view::npos)
		return false;
	std::uint64_t NV = 0;
	for(unsigned char Ch : Tail.substr(0, NewlineAfterCount)) {
		if(Ch < '0' || Ch > '9')
			return false;
		NV = NV * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(NewlineAfterCount + 1);
	for(std::uint64_t I = 0; I < NV; ++I) {
		const size_t NL1 = Tail.find('\n');
		if(NL1 == std::string_view::npos)
			return false;
		const std::string_view LenLine = Tail.substr(0, NL1);
		Tail = Tail.substr(NL1 + 1);
		const size_t Sp = LenLine.find(' ');
		if(Sp == std::string_view::npos)
			return false;
		std::uint64_t LN = 0, BN = 0;
		for(unsigned char Ch : LenLine.substr(0, Sp)) {
			if(Ch < '0' || Ch > '9')
				return false;
			LN = LN * 10 + static_cast<unsigned>(Ch - '0');
		}
		for(unsigned char Ch : LenLine.substr(Sp + 1)) {
			if(Ch < '0' || Ch > '9')
				return false;
			BN = BN * 10 + static_cast<unsigned>(Ch - '0');
		}
		if(Tail.size() < LN + 1 + BN)
			return false;
		std::string Name(Tail.substr(0, LN));
		if(Tail[LN] != '\n')
			return false;
		Tail = Tail.substr(LN + 1);
		std::string Body(Tail.substr(0, BN));
		if(Tail.size() < BN + 1 || Tail[BN] != '\n')
			return false;
		Tail = Tail.substr(BN + 1);
		OutViews[std::move(Name)] = std::move(Body);
	}
	RawData.erase(Mp);
	return true;
}

static constexpr std::string_view kSeqSnapshotMarkerSv = "<<<ASTRAL_DB_SEQUENCES>>>\n";

static void AppendSequenceSnapshotTrailer(std::string &RawData,
                                          const std::unordered_map<std::string, SequenceState> &Seqs) {
	if(Seqs.empty())
		return;
	RawData.append(kSeqSnapshotMarkerSv.data(), kSeqSnapshotMarkerSv.size());
	RawData += std::to_string(Seqs.size());
	RawData.push_back('\n');
	for(const auto &[Name, St] : Seqs) {
		RawData += std::to_string(Name.size());
		RawData.push_back(' ');
		RawData += std::to_string(4);
		RawData.push_back('\n');
		RawData += Name;
		RawData.push_back('\n');
		RawData += std::to_string(St.Start);
		RawData.push_back(' ');
		RawData += std::to_string(St.Increment);
		RawData.push_back(' ');
		RawData += std::to_string(St.Current);
		RawData.push_back('\n');
	}
}

static bool StripAndParseSequenceSnapshotTrailer(std::string &RawData,
                                                 std::unordered_map<std::string, SequenceState> &Out) {
	Out.clear();
	const size_t Mp = RawData.find(kSeqSnapshotMarkerSv.data(), 0, kSeqSnapshotMarkerSv.size());
	if(Mp == std::string::npos)
		return true;
	std::string_view Tail(RawData.data() + Mp + kSeqSnapshotMarkerSv.size(),
	                     RawData.size() - Mp - kSeqSnapshotMarkerSv.size());
	const size_t NL = Tail.find('\n');
	if(NL == std::string_view::npos)
		return false;
	std::uint64_t NV = 0;
	for(unsigned char Ch : Tail.substr(0, NL)) {
		if(Ch < '0' || Ch > '9')
			return false;
		NV = NV * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(NL + 1);
	for(std::uint64_t I = 0; I < NV; ++I) {
		const size_t NL1 = Tail.find('\n');
		if(NL1 == std::string_view::npos)
			return false;
		const std::string_view LenLine = Tail.substr(0, NL1);
		Tail = Tail.substr(NL1 + 1);
		const size_t Sp = LenLine.find(' ');
		if(Sp == std::string_view::npos)
			return false;
		std::uint64_t LN = 0;
		for(unsigned char Ch : LenLine.substr(0, Sp)) {
			if(Ch < '0' || Ch > '9')
				return false;
			LN = LN * 10 + static_cast<unsigned>(Ch - '0');
		}
		if(Tail.size() < LN + 1)
			return false;
		std::string Name(Tail.substr(0, LN));
		if(Tail[LN] != '\n')
			return false;
		Tail = Tail.substr(LN + 1);
		const size_t NL2 = Tail.find('\n');
		if(NL2 == std::string_view::npos)
			return false;
		SequenceState St;
		{
			std::istringstream RowIn(std::string(Tail.substr(0, NL2)));
			RowIn >> St.Start >> St.Increment >> St.Current;
			if(RowIn.fail())
				return false;
		}
		Tail = Tail.substr(NL2 + 1);
		Out[std::move(Name)] = St;
	}
	RawData.erase(Mp);
	return true;
}

static constexpr std::string_view kFkSnapshotMarkerSv = "<<<ASTRAL_DB_FOREIGN_KEYS>>>\n";

static void AppendForeignKeySnapshotTrailer(std::string &RawData,
                                            const std::unordered_map<std::string, std::vector<ForeignKey>> &Fks) {
	if(Fks.empty())
		return;
	std::vector<std::pair<std::string, ForeignKey>> Flat;
	for(const auto &[Tn, Vec] : Fks)
		for(const ForeignKey &Fk : Vec)
			Flat.emplace_back(Tn, Fk);
	RawData.append(kFkSnapshotMarkerSv.data(), kFkSnapshotMarkerSv.size());
	RawData += std::to_string(Flat.size());
	RawData.push_back('\n');
	for(const auto &[Tn, Fk] : Flat) {
		RawData += std::to_string(Tn.size());
		RawData.push_back(' ');
		RawData += std::to_string(Fk.ColumnName.size());
		RawData.push_back(' ');
		RawData += std::to_string(Fk.ReferencedTable.size());
		RawData.push_back(' ');
		RawData += std::to_string(Fk.ReferencedColumn.size());
		RawData.push_back(' ');
		RawData += std::to_string(static_cast<int>(Fk.OnDelete));
		RawData.push_back('\n');
		RawData += Tn;
		RawData.push_back('\n');
		RawData += Fk.ColumnName;
		RawData.push_back('\n');
		RawData += Fk.ReferencedTable;
		RawData.push_back('\n');
		RawData += Fk.ReferencedColumn;
		RawData.push_back('\n');
	}
}

static bool StripAndParseForeignKeySnapshotTrailer(std::string &RawData,
                                                   std::unordered_map<std::string, std::vector<ForeignKey>> &OutFks) {
	OutFks.clear();
	const size_t Mp = RawData.find(kFkSnapshotMarkerSv.data(), 0, kFkSnapshotMarkerSv.size());
	if(Mp == std::string::npos)
		return true;
	std::string_view Tail(RawData.data() + Mp + kFkSnapshotMarkerSv.size(),
	                      RawData.size() - Mp - kFkSnapshotMarkerSv.size());
	const size_t NlCnt = Tail.find('\n');
	if(NlCnt == std::string_view::npos)
		return false;
	std::uint64_t NF = 0;
	for(unsigned char Ch : Tail.substr(0, NlCnt)) {
		if(Ch < '0' || Ch > '9')
			return false;
		NF = NF * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(NlCnt + 1);
	for(std::uint64_t I = 0; I < NF; ++I) {
		const size_t LnHdr = Tail.find('\n');
		if(LnHdr == std::string_view::npos)
			return false;
		std::istringstream Ls(std::string(Tail.substr(0, LnHdr)));
		Tail = Tail.substr(LnHdr + 1);
		std::uint64_t LTn = 0, LCol = 0, LRt = 0, LRc = 0;
		long long Act = 0;
		if(!(Ls >> LTn >> LCol >> LRt >> LRc >> Act))
			return false;
		if(Act < 0 || Act > 2)
			return false;
		if(Tail.size() < LTn + 1 + LCol + 1 + LRt + 1 + LRc + 1)
			return false;
		std::string Tn(Tail.substr(0, LTn));
		if(Tail[LTn] != '\n')
			return false;
		Tail = Tail.substr(LTn + 1);
		std::string Col(Tail.substr(0, LCol));
		if(Tail[LCol] != '\n')
			return false;
		Tail = Tail.substr(LCol + 1);
		std::string Rt(Tail.substr(0, LRt));
		if(Tail[LRt] != '\n')
			return false;
		Tail = Tail.substr(LRt + 1);
		std::string Rc(Tail.substr(0, LRc));
		if(Tail[LRc] != '\n')
			return false;
		Tail = Tail.substr(LRc + 1);
		ForeignKey Fk;
		Fk.ColumnName = std::move(Col);
		Fk.ReferencedTable = std::move(Rt);
		Fk.ReferencedColumn = std::move(Rc);
		Fk.OnDelete = static_cast<ReferentialAction>(static_cast<uint8_t>(Act));
		OutFks[Tn].push_back(std::move(Fk));
	}
	RawData.erase(Mp);
	return true;
}

static constexpr std::string_view kUserAclSnapshotMarkerSv = "<<<ASTRAL_DB_USER_ACL>>>\n";

struct SecuritySnapshotExtras {
	std::unordered_set<std::string> Roles;
	std::unordered_map<std::string, std::unordered_map<std::string, Permissions>> RoleAcls;
	std::unordered_map<std::string, std::vector<std::string>> UserRoles;
	std::vector<std::pair<std::string, RowColPermission>> FineGrants;
};

static void AppendUserAclSnapshotTrailer(std::string &RawData, const std::vector<User> &Users,
    const std::unordered_map<std::string, std::unordered_map<std::string, Permissions>> &Acls,
    const SecuritySnapshotExtras &Extras) {
	RawData.append(kUserAclSnapshotMarkerSv.data(), kUserAclSnapshotMarkerSv.size());
	RawData += std::to_string(Users.size());
	RawData.push_back('\n');
	for(const auto &U : Users) {
		const std::string &Nm = U.Name;
		const std::string Blob = U.Password.Encrypted();
		const auto &KeyArr = U.Password.EncryptionKey();
		const std::string KeyStr(reinterpret_cast<const char *>(KeyArr.data()), KeyArr.size());
		RawData += std::to_string(Nm.size());
		RawData.push_back(' ');
		RawData += std::to_string(Blob.size());
		RawData.push_back(' ');
		RawData += std::to_string(KeyStr.size());
		RawData.push_back('\n');
		RawData += Nm;
		RawData.push_back('\n');
		RawData += Blob;
		RawData.push_back('\n');
		RawData += KeyStr;
		RawData.push_back('\n');
	}
	std::vector<std::tuple<std::string, std::string, int>> Flat;
	for(const auto &[Un, Inner] : Acls)
		for(const auto &[Tn, Pm] : Inner)
			Flat.emplace_back(Un, Tn, static_cast<int>(Pm));
	RawData += std::to_string(Flat.size());
	RawData.push_back('\n');
	for(const auto &[Un, Tn, Pm] : Flat) {
		RawData += std::to_string(Un.size());
		RawData.push_back(' ');
		RawData += std::to_string(Tn.size());
		RawData.push_back(' ');
		RawData += std::to_string(static_cast<long long>(Pm));
		RawData.push_back('\n');
		RawData += Un;
		RawData.push_back('\n');
		RawData += Tn;
		RawData.push_back('\n');
	}
	RawData += std::to_string(Extras.Roles.size());
	RawData.push_back('\n');
	for(const std::string &Role : Extras.Roles) {
		RawData += std::to_string(Role.size());
		RawData.push_back('\n');
		RawData += Role;
		RawData.push_back('\n');
	}
	std::vector<std::tuple<std::string, std::string, int>> RoleFlat;
	for(const auto &[Rn, Inner] : Extras.RoleAcls)
		for(const auto &[Tn, Pm] : Inner)
			RoleFlat.emplace_back(Rn, Tn, static_cast<int>(Pm));
	RawData += std::to_string(RoleFlat.size());
	RawData.push_back('\n');
	for(const auto &[Rn, Tn, Pm] : RoleFlat) {
		RawData += std::to_string(Rn.size());
		RawData.push_back(' ');
		RawData += std::to_string(Tn.size());
		RawData.push_back(' ');
		RawData += std::to_string(static_cast<long long>(Pm));
		RawData.push_back('\n');
		RawData += Rn;
		RawData.push_back('\n');
		RawData += Tn;
		RawData.push_back('\n');
	}
	std::vector<std::pair<std::string, std::string>> MembershipFlat;
	for(const auto &[Un, Roles] : Extras.UserRoles)
		for(const std::string &Rn : Roles)
			MembershipFlat.emplace_back(Un, Rn);
	RawData += std::to_string(MembershipFlat.size());
	RawData.push_back('\n');
	for(const auto &[Un, Rn] : MembershipFlat) {
		RawData += std::to_string(Un.size());
		RawData.push_back(' ');
		RawData += std::to_string(Rn.size());
		RawData.push_back('\n');
		RawData += Un;
		RawData.push_back('\n');
		RawData += Rn;
		RawData.push_back('\n');
	}
	RawData += std::to_string(Extras.FineGrants.size());
	RawData.push_back('\n');
	for(const auto &[Un, Rule] : Extras.FineGrants) {
		RawData += std::to_string(Un.size());
		RawData.push_back(' ');
		RawData += std::to_string(Rule.Table.size());
		RawData.push_back(' ');
		RawData += std::to_string(Rule.RowId.size());
		RawData.push_back(' ');
		RawData += std::to_string(Rule.Column.size());
		RawData.push_back(' ');
		RawData += std::to_string(static_cast<long long>(static_cast<int>(Rule.Perms)));
		RawData.push_back('\n');
		RawData += Un;
		RawData.push_back('\n');
		RawData += Rule.Table;
		RawData.push_back('\n');
		RawData += Rule.RowId;
		RawData.push_back('\n');
		RawData += Rule.Column;
		RawData.push_back('\n');
	}
}

static bool ReadUint64Line(std::string_view &Tail, std::uint64_t &Out) {
	const size_t Nl = Tail.find('\n');
	if(Nl == std::string_view::npos)
		return false;
	Out = 0;
	for(unsigned char Ch : Tail.substr(0, Nl)) {
		if(Ch < '0' || Ch > '9')
			return false;
		Out = Out * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(Nl + 1);
	return true;
}

static bool StripAndParseUserAclSnapshotTrailer(std::string &RawData, bool &OutPresent, std::vector<User> &OutUsers,
    std::unordered_map<std::string, std::unordered_map<std::string, Permissions>> &OutAcls,
    SecuritySnapshotExtras &OutExtras) {
	OutPresent = false;
	OutUsers.clear();
	OutAcls.clear();
	OutExtras = SecuritySnapshotExtras{};
	const size_t Mp = RawData.find(kUserAclSnapshotMarkerSv.data(), 0, kUserAclSnapshotMarkerSv.size());
	if(Mp == std::string::npos)
		return true;
	std::string_view Tail(RawData.data() + Mp + kUserAclSnapshotMarkerSv.size(),
	                      RawData.size() - Mp - kUserAclSnapshotMarkerSv.size());
	const size_t NlUsers = Tail.find('\n');
	if(NlUsers == std::string_view::npos)
		return false;
	std::uint64_t NU = 0;
	for(unsigned char Ch : Tail.substr(0, NlUsers)) {
		if(Ch < '0' || Ch > '9')
			return false;
		NU = NU * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(NlUsers + 1);
	if(NU == 0)
		return false;
	OutPresent = true;
	for(std::uint64_t I = 0; I < NU; ++I) {
		const size_t Ln1 = Tail.find('\n');
		if(Ln1 == std::string_view::npos)
			return false;
		const std::string_view LenLine = Tail.substr(0, Ln1);
		Tail = Tail.substr(Ln1 + 1);
		const size_t Sp1 = LenLine.find(' ');
		const size_t Sp2 = LenLine.find(' ', Sp1 == std::string_view::npos ? 0 : Sp1 + 1);
		if(Sp1 == std::string_view::npos || Sp2 == std::string_view::npos)
			return false;
		std::uint64_t LN = 0, BN = 0, KN = 0;
		for(unsigned char Ch : LenLine.substr(0, Sp1)) {
			if(Ch < '0' || Ch > '9')
				return false;
			LN = LN * 10 + static_cast<unsigned>(Ch - '0');
		}
		for(unsigned char Ch : LenLine.substr(Sp1 + 1, Sp2 - Sp1 - 1)) {
			if(Ch < '0' || Ch > '9')
				return false;
			BN = BN * 10 + static_cast<unsigned>(Ch - '0');
		}
		for(unsigned char Ch : LenLine.substr(Sp2 + 1)) {
			if(Ch < '0' || Ch > '9')
				return false;
			KN = KN * 10 + static_cast<unsigned>(Ch - '0');
		}
		if(KN != 32 || Tail.size() < LN + 1 + BN + 1 + KN + 1)
			return false;
		std::string Name(Tail.substr(0, LN));
		if(Tail[LN] != '\n')
			return false;
		Tail = Tail.substr(LN + 1);
		std::string Blob(Tail.substr(0, BN));
		if(Tail[BN] != '\n')
			return false;
		Tail = Tail.substr(BN + 1);
		std::string KeyStr(Tail.substr(0, KN));
		if(Tail[KN] != '\n')
			return false;
		Tail = Tail.substr(KN + 1);
		std::array<uint8_t, 32> K{};
		std::memcpy(K.data(), KeyStr.data(), 32);
		EncryptedString Es(std::move(Blob), K);
		OutUsers.emplace_back(std::move(Name), std::move(Es));
	}
	const size_t NlAcl = Tail.find('\n');
	if(NlAcl == std::string_view::npos)
		return false;
	std::uint64_t NA = 0;
	for(unsigned char Ch : Tail.substr(0, NlAcl)) {
		if(Ch < '0' || Ch > '9')
			return false;
		NA = NA * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(NlAcl + 1);
	for(std::uint64_t I = 0; I < NA; ++I) {
		const size_t LnA = Tail.find('\n');
		if(LnA == std::string_view::npos)
			return false;
		const std::string LenLineStr(Tail.substr(0, LnA));
		Tail = Tail.substr(LnA + 1);
		std::istringstream Ls(LenLineStr);
		std::uint64_t UL = 0, TL = 0;
		long long PM = 0;
		if(!(Ls >> UL >> TL >> PM))
			return false;
		if(Tail.size() < UL + 1 + TL + 1)
			return false;
		std::string Un(Tail.substr(0, UL));
		if(Tail[UL] != '\n')
			return false;
		Tail = Tail.substr(UL + 1);
		std::string Tn(Tail.substr(0, TL));
		if(Tail[TL] != '\n')
			return false;
		Tail = Tail.substr(TL + 1);
		OutAcls[std::move(Un)][std::move(Tn)] = static_cast<Permissions>(static_cast<int>(PM));
	}
	std::uint64_t NR = 0;
	if(ReadUint64Line(Tail, NR)) {
		for(std::uint64_t I = 0; I < NR; ++I) {
			const size_t Ln = Tail.find('\n');
			if(Ln == std::string_view::npos)
				return false;
			std::uint64_t LN = 0;
			for(unsigned char Ch : Tail.substr(0, Ln)) {
				if(Ch < '0' || Ch > '9')
					return false;
				LN = LN * 10 + static_cast<unsigned>(Ch - '0');
			}
			Tail = Tail.substr(Ln + 1);
			if(Tail.size() < LN + 1)
				return false;
			OutExtras.Roles.insert(std::string(Tail.substr(0, LN)));
			if(Tail[LN] != '\n')
				return false;
			Tail = Tail.substr(LN + 1);
		}
		std::uint64_t NRA = 0;
		if(!ReadUint64Line(Tail, NRA))
			return false;
		for(std::uint64_t I = 0; I < NRA; ++I) {
			const size_t LnA = Tail.find('\n');
			if(LnA == std::string_view::npos)
				return false;
			std::istringstream Ls(std::string(Tail.substr(0, LnA)));
			Tail = Tail.substr(LnA + 1);
			std::uint64_t RL = 0, TL = 0;
			long long PM = 0;
			if(!(Ls >> RL >> TL >> PM))
				return false;
			if(Tail.size() < RL + 1 + TL + 1)
				return false;
			std::string Rn(Tail.substr(0, RL));
			if(Tail[RL] != '\n')
				return false;
			Tail = Tail.substr(RL + 1);
			std::string Tn(Tail.substr(0, TL));
			if(Tail[TL] != '\n')
				return false;
			Tail = Tail.substr(TL + 1);
			OutExtras.RoleAcls[std::move(Rn)][std::move(Tn)] = static_cast<Permissions>(static_cast<int>(PM));
		}
		std::uint64_t NUR = 0;
		if(!ReadUint64Line(Tail, NUR))
			return false;
		for(std::uint64_t I = 0; I < NUR; ++I) {
			const size_t LnM = Tail.find('\n');
			if(LnM == std::string_view::npos)
				return false;
			const std::string_view LenLine = Tail.substr(0, LnM);
			Tail = Tail.substr(LnM + 1);
			const size_t Sp = LenLine.find(' ');
			if(Sp == std::string_view::npos)
				return false;
			std::uint64_t UL = 0, RL = 0;
			for(unsigned char Ch : LenLine.substr(0, Sp)) {
				if(Ch < '0' || Ch > '9')
					return false;
				UL = UL * 10 + static_cast<unsigned>(Ch - '0');
			}
			for(unsigned char Ch : LenLine.substr(Sp + 1)) {
				if(Ch < '0' || Ch > '9')
					return false;
				RL = RL * 10 + static_cast<unsigned>(Ch - '0');
			}
			if(Tail.size() < UL + 1 + RL + 1)
				return false;
			std::string Un(Tail.substr(0, UL));
			if(Tail[UL] != '\n')
				return false;
			Tail = Tail.substr(UL + 1);
			std::string Rn(Tail.substr(0, RL));
			if(Tail[RL] != '\n')
				return false;
			Tail = Tail.substr(RL + 1);
			OutExtras.UserRoles[Un].push_back(std::move(Rn));
		}
		std::uint64_t NF = 0;
		if(!ReadUint64Line(Tail, NF))
			return false;
		for(std::uint64_t I = 0; I < NF; ++I) {
			const size_t LnF = Tail.find('\n');
			if(LnF == std::string_view::npos)
				return false;
			std::istringstream Ls(std::string(Tail.substr(0, LnF)));
			Tail = Tail.substr(LnF + 1);
			std::uint64_t UL = 0, TL = 0, RL = 0, CL = 0;
			long long PM = 0;
			if(!(Ls >> UL >> TL >> RL >> CL >> PM))
				return false;
			if(Tail.size() < UL + 1 + TL + 1 + RL + 1 + CL + 1)
				return false;
			std::string Un(Tail.substr(0, UL));
			if(Tail[UL] != '\n')
				return false;
			Tail = Tail.substr(UL + 1);
			RowColPermission Rule;
			Rule.Table.assign(Tail.substr(0, TL));
			if(Tail[TL] != '\n')
				return false;
			Tail = Tail.substr(TL + 1);
			Rule.RowId.assign(Tail.substr(0, RL));
			if(Tail[RL] != '\n')
				return false;
			Tail = Tail.substr(RL + 1);
			Rule.Column.assign(Tail.substr(0, CL));
			if(Tail[CL] != '\n')
				return false;
			Tail = Tail.substr(CL + 1);
			Rule.Perms = static_cast<Permissions>(static_cast<int>(PM));
			OutExtras.FineGrants.emplace_back(std::move(Un), std::move(Rule));
		}
	}
	RawData.erase(Mp);
	return true;
}

} // namespace

namespace {

constexpr std::string_view kObjectTypeSnapshotMarker = "\n__ASTRAL_OBJECT_TYPES__\n";

void AppendObjectTypeSnapshotTrailer(std::string &RawData,
                                     const std::unordered_map<std::string, Database::ObjectTypeSchema> &Types,
                                     const std::unordered_map<std::string, std::string> &TypedBindings) {
	std::ostringstream O;
	O << kObjectTypeSnapshotMarker << Types.size() << '\n';
	for(const auto &Pr : Types) {
		O << WalEncodeSqlBody(Pr.first) << '\n' << Pr.second.size() << '\n';
		for(const auto &F : Pr.second)
			O << WalEncodeSqlBody(F.Name) << '\n' << WalEncodeSqlBody(F.Type) << '\n';
	}
	O << TypedBindings.size() << '\n';
	for(const auto &Pr : TypedBindings)
		O << WalEncodeSqlBody(Pr.first) << '\n' << WalEncodeSqlBody(Pr.second) << '\n';
	RawData += O.str();
}

bool StripAndParseObjectTypeSnapshotTrailer(std::string &RawData,
                                            std::unordered_map<std::string, Database::ObjectTypeSchema> &OutTypes,
                                            std::unordered_map<std::string, std::string> &OutTypedBindings) {
	const size_t Pos = RawData.rfind(kObjectTypeSnapshotMarker);
	if(Pos == std::string::npos)
		return true;
	std::istringstream In(RawData.substr(Pos + kObjectTypeSnapshotMarker.size()));
	size_t TypeCount = 0;
	In >> TypeCount;
	In.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	for(size_t I = 0; I < TypeCount; ++I) {
		std::string TypeNameEnc;
		if(!std::getline(In, TypeNameEnc))
			return false;
		size_t FieldCount = 0;
		if(!(In >> FieldCount))
			return false;
		In.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		Database::ObjectTypeSchema Fields;
		for(size_t J = 0; J < FieldCount; ++J) {
			std::string FNameEnc;
			std::string FTypeEnc;
			if(!std::getline(In, FNameEnc) || !std::getline(In, FTypeEnc))
				return false;
			Fields.push_back({WalDecodeSqlBodyB64(FNameEnc), WalDecodeSqlBodyB64(FTypeEnc)});
		}
		OutTypes[WalDecodeSqlBodyB64(TypeNameEnc)] = std::move(Fields);
	}
	size_t BindingCount = 0;
	if(!(In >> BindingCount))
		return false;
	In.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	for(size_t I = 0; I < BindingCount; ++I) {
		std::string TableEnc;
		std::string TypeEnc;
		if(!std::getline(In, TableEnc) || !std::getline(In, TypeEnc))
			return false;
		OutTypedBindings[WalDecodeSqlBodyB64(TableEnc)] = WalDecodeSqlBodyB64(TypeEnc);
	}
	RawData.erase(Pos);
	return true;
}

} // namespace

void Database::AppendWalAfterSetTableStorage(const std::string &TableName, StorageLayout Layout) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "ST|" << WalEncodeSqlBody(TableName) << '|' << static_cast<int>(Layout);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterCreate(const std::string &TableName, const Schema &Columns) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "T|" << TableName << '|' << Columns.size();
	for(const auto &Col : Columns) {
		O << '|' << Col.Name << '|' << Col.DefaultValue << '|'
		  << (Col.IsPrimaryKey ? "1" : "0") << '|'
		  << (Col.IsUnique ? "1" : "0") << '|'
		  << (Col.IsNotNull ? "1" : "0") << '|' << WalEncodeOptionalConstraintB64(Col.CheckConstraintSql) << '|'
		  << WalEncodeOptionalConstraintB64(Col.CheckConstraintDnfPacked) << '|'
		  << (Col.IsIdentity ? (Col.IdentityAlways ? "1" : "0") : "-") << '|'
		  << (Col.IsIdentity && !Col.IdentitySequenceName.empty() ? Col.IdentitySequenceName : "-");
	}
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterCreateSequence(const std::string &Name, int64_t Start, int64_t Increment) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "SQ|" << Name << '|' << Start << '|' << Increment;
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterDropSequence(const std::string &Name) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("SD|") + Name);
}

void Database::AppendWalAfterCreateType(const std::string &TypeName, const ObjectTypeSchema &Fields) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "TY|" << WalEncodeSqlBody(TypeName) << '|' << Fields.size();
	for(const auto &F : Fields)
		O << '|' << WalEncodeSqlBody(F.Name) << '|' << WalEncodeSqlBody(F.Type);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterDropType(const std::string &TypeName) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("TDY|") + WalEncodeSqlBody(TypeName));
}

void Database::AppendWalAfterBindTypedTable(const std::string &TableName, const std::string &TypeName) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("BT|") + WalEncodeSqlBody(TableName) + "|" + WalEncodeSqlBody(TypeName));
}

void Database::AppendWalAfterInsert(const std::string &TableName, const Item &Row) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "I|" << TableName << '|' << Row.size();
	for(const auto &[Key, Val] : Row)
		O << '|' << Key << '|' << Val;
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterDrop(const std::string &TableName) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("D|") + TableName);
}

void Database::AppendWalAfterDefineView(const std::string &ViewName, const std::string &SqlBody) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << 'V' << '|' << ViewName << '|' << WalEncodeSqlBody(SqlBody);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterDropView(const std::string &ViewName) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("DV|") + ViewName);
}

void Database::AppendWalAfterDefineProcedure(const std::string &Name, const std::string &SqlBody) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "PR|" << Name << '|' << WalEncodeSqlBody(SqlBody);
	Wal_.AppendLine(O.str());
}


void Database::AppendWalAfterDefineTrigger(const std::string &Name, const std::string &SpecJson) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "TR|" << Name << '|' << WalEncodeSqlBody(SpecJson);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterDropTrigger(const std::string &Name) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("TD|") + Name);
}

void Database::AppendWalAfterSetTriggerEnabled(const std::string &Name, bool Enabled) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string(Enabled ? "TE|" : "TX|") + Name);
}

void Database::AppendWalAfterDropProcedure(const std::string &Name) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("PD|") + Name);
}

void Database::AppendWalAfterAddUser(const User &U) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	const std::string KeyStr(reinterpret_cast<const char *>(U.Password.EncryptionKey().data()),
	                         U.Password.EncryptionKey().size());
	O << "UU|" << WalEncodeSqlBody(U.Name) << '|' << WalEncodeSqlBody(U.Password.Encrypted()) << '|'
	  << WalEncodeSqlBody(KeyStr);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterRemoveUser(const std::string &Name) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "UD|" << WalEncodeSqlBody(Name);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterGrantAcl(const std::string &UserName, const std::string &Table, int Bits) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "UG|" << WalEncodeSqlBody(UserName) << '|' << WalEncodeSqlBody(Table) << '|' << Bits;
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterRevokeAcl(const std::string &UserName, const std::string &Table, int Bits) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "UR|" << WalEncodeSqlBody(UserName) << '|' << WalEncodeSqlBody(Table) << '|' << Bits;
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterCreateRole(const std::string &RoleName) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("CR|") + WalEncodeSqlBody(RoleName));
}

void Database::AppendWalAfterDropRole(const std::string &RoleName) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(std::string("DR|") + WalEncodeSqlBody(RoleName));
}

void Database::AppendWalAfterGrantRoleMembership(const std::string &RoleName, const std::string &UserName) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "GM|" << WalEncodeSqlBody(RoleName) << '|' << WalEncodeSqlBody(UserName);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterRevokeRoleMembership(const std::string &RoleName, const std::string &UserName) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "RM|" << WalEncodeSqlBody(RoleName) << '|' << WalEncodeSqlBody(UserName);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterGrantRoleAcl(const std::string &RoleName, const std::string &Table, int Bits) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "RG|" << WalEncodeSqlBody(RoleName) << '|' << WalEncodeSqlBody(Table) << '|' << Bits;
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterRevokeRoleAcl(const std::string &RoleName, const std::string &Table, int Bits) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "RR|" << WalEncodeSqlBody(RoleName) << '|' << WalEncodeSqlBody(Table) << '|' << Bits;
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterFineGrant(const std::string &UserName, const RowColPermission &Rule) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "UF|" << WalEncodeSqlBody(UserName) << '|' << WalEncodeSqlBody(Rule.Table) << '|'
	  << WalEncodeSqlBody(Rule.RowId) << '|' << WalEncodeSqlBody(Rule.Column) << '|'
	  << static_cast<int>(Rule.Perms);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterFineRevoke(const std::string &UserName, const RowColPermission &Rule) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << "XF|" << WalEncodeSqlBody(UserName) << '|' << WalEncodeSqlBody(Rule.Table) << '|'
	  << WalEncodeSqlBody(Rule.RowId) << '|' << WalEncodeSqlBody(Rule.Column) << '|'
	  << static_cast<int>(Rule.Perms);
	Wal_.AppendLine(O.str());
}

void Database::AppendWalAfterForeignKey(const std::string &TableName, const ForeignKey &Key) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	std::ostringstream O;
	O << 'F' << '|' << WalEncodeSqlBody(TableName) << '|' << WalEncodeSqlBody(Key.ColumnName) << '|'
	  << WalEncodeSqlBody(Key.ReferencedTable) << '|' << WalEncodeSqlBody(Key.ReferencedColumn) << '|'
	  << static_cast<int>(Key.OnDelete);
	Wal_.AppendLine(O.str());
}

Database::~Database() {
	/** \c DbDispatchAsync runs \c std::async jobs holding \c this; join flush only after workers finish */
	while(OutstandingAsyncJobs_.load(std::memory_order_acquire) != 0)
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	StopFlushWorker_.store(true, std::memory_order_release);
	JoinFlushWorkerBestEffort();
	if(!SkipExitSyncOnDestroy_ && Dirty_.load(std::memory_order_acquire)) {
		try {
			SyncToFile();
		} catch(...) {}
	}
	if(Logger_) Logger_->Info("Database destroyed");
}

std::string Database::CompressData(const std::string &Data) {
	return DS::LZ4Compress(Data);
}

std::string Database::DecompressData(const std::string &CompressedData) {
	return DS::LZ4Decompress(CompressedData);
}

std::string Database::EncryptData(const std::string &Data) {
	return SealAtRestPayload(Data);
}

std::string Database::DecryptData(const std::string &EncryptedData) {
	return UnsealAtRestPayload(EncryptedData);
}

void Database::SyncToFileUnlocked() {
	std::ostringstream OutputStream;
	OutputStream << TableSchemas_.size() << "\n";
	for(const auto &SchemaPair : TableSchemas_) {
		OutputStream << SchemaPair.first << "\n";
		OutputStream << SchemaPair.second.size() << "\n";
		for(const auto &Column : SchemaPair.second) {
			OutputStream << Column.Name << " " << Column.IsPrimaryKey << " " << Column.IsUnique << " "
			             << Column.IsNotNull << " " << Column.DefaultValue << " "
			             << WalEncodeOptionalConstraintB64(Column.CheckConstraintSql) << " "
			             << WalEncodeOptionalConstraintB64(Column.CheckConstraintDnfPacked) << " ";
			if(Column.IsIdentity)
				OutputStream << (Column.IdentityAlways ? "1" : "0") << " "
				             << (Column.IdentitySequenceName.empty() ? "-" : Column.IdentitySequenceName);
			else
				OutputStream << "- -";
			OutputStream << "\n";
		}
	}
	OutputStream << Tables_.size() << "\n";
	for(const auto &TablePair : Tables_) {
		OutputStream << TablePair.first << "\n";  // Table name
		OutputStream << TablePair.second.RowStore.size() << "\n";  // Row count
		for(const auto &Row : TablePair.second.RowStore) {
			OutputStream << Row.size() << "\n";  // Column count per row
			for (const auto &Column : Row) {
				OutputStream << Column.first << "\n";  // Column name
				OutputStream << Column.second << "\n";  // Column value
			}
		}
	}
	std::string RawData = OutputStream.str();
	SecuritySnapshotExtras Extras;
	Extras.Roles = Roles_;
	Extras.RoleAcls = RoleAcls_;
	Extras.UserRoles = UserRoles_;
	for(const User &U : Users_) {
		for(const RowColPermission &Rule : U.FineGrainedPermissions)
			Extras.FineGrants.emplace_back(U.Name, Rule);
	}
	AppendUserAclSnapshotTrailer(RawData, Users_, Acls_, Extras);
	AppendForeignKeySnapshotTrailer(RawData, ForeignKeys_);
	AppendSequenceSnapshotTrailer(RawData, Sequences_);
	AppendStorageSnapshotTrailer(RawData, Tables_);
	AppendViewSnapshotTrailer(RawData, ViewDefinitionSql_);
	AppendTriggerSnapshotTrailer(RawData, TriggerDefinitions_);
	AppendGraphCatalogSnapshotTrailer(RawData, Graphs_);
	AppendEmbeddingCatalogSnapshotTrailer(RawData, Embeddings_);
	AppendObjectTypeSnapshotTrailer(RawData, ObjectTypes_, TypedTableBindings_);
	std::string CompressedData = CompressData(RawData);
	std::string EncryptedData = EncryptData(CompressedData);
	std::ofstream File(DbPath_, std::ios::binary);
	if(!File) {
		if(Logger_) Logger_->Error("Failed to open file for writing in SyncToFile");
		FailStorage("Cannot open database file for writing (path: " + DbPath_.string() +
		            "). Check permissions and that the directory exists.");
	}
	File.write(EncryptedData.data(), EncryptedData.size());
	Wal_.Truncate();
	if(Logger_) Logger_->Info("Database synced to file");
}

void Database::SyncToFile() {
	std::scoped_lock Guard(DbMutex_);
	SyncToFileUnlocked();
}

namespace {

constexpr std::streamoff kMaxDbSnapshotCopyBytes = 512LL * 1024 * 1024;

/** Read-write whole file (\c trunc) on \a Dst; used only while \c DbMutex_ already held. */
static void CopyWholeFileOverwriteForSnapshot(const std::filesystem::path &Src, const std::filesystem::path &Dst) {
	std::ifstream In(Src, std::ios::binary | std::ios::ate);
	if(!In)
		FailStorage("Cannot open source file \"" + Src.string() + "\" for whole-file snapshot copy.");
	std::streamoff Sz = In.tellg();
	if(Sz <= 0)
		FailStorage("Source file \"" + Src.string() + "\" is empty or unreadable.");
	if(Sz > kMaxDbSnapshotCopyBytes)
		FailStorage("Source database file \"" + Src.string() + "\" is unexpectedly large.");
	if(static_cast<unsigned long long>(Sz) >
	   static_cast<unsigned long long>(std::numeric_limits<std::streamsize>::max()))
		FailStorage("Source file size overflow.");
	In.seekg(0);
	std::vector<char> Buf(static_cast<size_t>(Sz));
	In.read(Buf.data(), Sz);
	if(!In || In.gcount() != Sz)
		FailStorage("Short read copying database file \"" + Src.string() + "\" for snapshot.");
	std::ofstream Out(Dst, std::ios::binary | std::ios::trunc);
	if(!Out)
		FailStorage("Cannot open destination \"" + Dst.string() + "\" for database snapshot overwrite.");
	Out.write(Buf.data(), static_cast<std::streamsize>(Buf.size()));
	Out.flush();
	if(!Out)
		FailStorage("Incomplete write writing database snapshot \"" + Dst.string() + "\".");
}

} // namespace

void Database::SyncToFileAndCopyMainDbFileTo(const std::filesystem::path &SnapshotPath) {
	std::scoped_lock<SharedMutex> Guard(DbMutex_);
	SyncToFileUnlocked();
	CopyWholeFileOverwriteForSnapshot(DbPath_, SnapshotPath);
}

void Database::FlushWalToDisk() {
	Wal_.Flush();
}

void Database::QuiesceBackgroundIOForFilesystemRollback() noexcept {
	while(OutstandingAsyncJobs_.load(std::memory_order_acquire) != 0)
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	StopFlushWorker_.store(true, std::memory_order_release);
	JoinFlushWorkerBestEffort();
}

void Database::ClearDirtyForFilesystemRollback() {
	std::scoped_lock Guard(DbMutex_);
	Dirty_.store(false, std::memory_order_release);
}

void Database::CloneTable(const std::string &Dest, const std::string &Src) {
	std::scoped_lock Guard(DbMutex_);
	if(AstralDB::SQL::IsDialectDualTable(Src)) {
		if(Dest.empty())
			FailStorage("CLONE TABLE: destination name must be non-empty.");
		TableSchemas_[Dest] = {};
		Tables_[Dest] = HybridTableSlot{};
		Tables_[Dest].RowStore = {Item{}};
		Tables_[Dest].RecordWrite();
		Indexes_.erase(Dest);
		ForeignKeys_.erase(Dest);
		TableCheckConstraints_.erase(Dest);
		Dirty_.store(true, std::memory_order_release);
		return;
	}
	auto TIt = Tables_.find(Src);
	auto SIt = TableSchemas_.find(Src);
		if(TIt == Tables_.end() || SIt == TableSchemas_.end())
			FailStorage("CLONE TABLE: source table \"" + Src + "\" does not exist in this database.");
		if(Dest.empty() || Src.empty())
			FailStorage("CLONE TABLE: source and destination names must be non-empty.");
	TableSchemas_[Dest] = SIt->second;
	Tables_[Dest] = TIt->second;
	Tables_[Dest].ColumnarSynced = false;
	Indexes_.erase(Dest);
	ForeignKeys_.erase(Dest);
	auto Ck = TableCheckConstraints_.find(Src);
	if(Ck != TableCheckConstraints_.end())
		TableCheckConstraints_[Dest] = Ck->second;
	else
		TableCheckConstraints_.erase(Dest);
	Dirty_.store(true, std::memory_order_release);
}

std::unordered_map<std::string, std::string> Database::ViewDefinitionsSnapshot() const {
	std::scoped_lock Guard(DbMutex_);
	return ViewDefinitionSql_;
}

bool Database::HasViewDefinition(const std::string &ViewName) const {
	std::scoped_lock Guard(DbMutex_);
	return ViewDefinitionSql_.find(ViewName) != ViewDefinitionSql_.end();
}

void Database::DefineView(const std::string &ViewName, std::string SqlBody) {
	std::scoped_lock Guard(DbMutex_);
	if(ViewName.empty())
		FailStorage("CREATE VIEW: view name must be non-empty.");
	if(TableSchemas_.find(ViewName) != TableSchemas_.end())
		FailStorage("CREATE VIEW: a table named \"" + ViewName + "\" already exists.");
	if(ViewDefinitionSql_.find(ViewName) != ViewDefinitionSql_.end())
		FailStorage("CREATE VIEW: view \"" + ViewName + "\" already exists.");
	ViewDefinitionSql_[ViewName] = std::move(SqlBody);
	AppendWalAfterDefineView(ViewName, ViewDefinitionSql_[ViewName]);
	Dirty_.store(true, std::memory_order_release);
}

void Database::DropViewDefinition(const std::string &ViewName, bool IfExists) {
	std::scoped_lock Guard(DbMutex_);
	auto It = ViewDefinitionSql_.find(ViewName);
	if(It == ViewDefinitionSql_.end()) {
		if(!IfExists)
			FailStorage("DROP VIEW: view \"" + ViewName + "\" does not exist.");
		return;
	}
	ViewDefinitionSql_.erase(It);
	AppendWalAfterDropView(ViewName);
	Dirty_.store(true, std::memory_order_release);
}

void Database::ReplayWalDefineView(const std::string &ViewName, std::string SqlBody) {
	std::scoped_lock Guard(DbMutex_);
	if(ViewName.empty())
		FailStorage("WAL replay CREATE VIEW: empty name.");
	if(TableSchemas_.find(ViewName) != TableSchemas_.end())
		FailStorage("WAL replay CREATE VIEW: name \"" + ViewName + "\" collides with existing table.");
	ViewDefinitionSql_[ViewName] = std::move(SqlBody);
}

void Database::ReplayWalDropView(const std::string &ViewName) {
	std::scoped_lock Guard(DbMutex_);
	ViewDefinitionSql_.erase(ViewName);
}

std::unordered_map<std::string, std::string> Database::ProcedureDefinitionsSnapshot() const {
	std::scoped_lock Guard(DbMutex_);
	return ProcedureDefinitionSql_;
}

void Database::DefineProcedure(const std::string &ProcedureName, std::string SqlBody, bool IfNotExists, bool OrReplace,
                               std::string SourceDialect, std::string ExceptionHandlersJson) {
	const auto ExceptionHandlers = SQL::DecodeExceptionHandlersJson(ExceptionHandlersJson);
	std::string BodyForCache;
	{
		std::scoped_lock Guard(DbMutex_);
		if(ProcedureName.empty())
			FailStorage("CREATE PROCEDURE: name must be non-empty.");
		const auto Existing = ProcedureDefinitionSql_.find(ProcedureName);
		if(Existing != ProcedureDefinitionSql_.end()) {
			if(IfNotExists)
				return;
			if(!OrReplace)
				FailStorage("CREATE PROCEDURE: \"" + ProcedureName + "\" already exists.");
			ProcedureDefinitionSql_.erase(Existing);
		}
		ProcedureDefinitionSql_[ProcedureName] = SqlBody;
		BodyForCache = ProcedureDefinitionSql_[ProcedureName];
		AppendWalAfterDefineProcedure(ProcedureName, BodyForCache);
		Dirty_.store(true, std::memory_order_release);
	}
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	SQL::UnregisterProcedure(Catalog, ProcedureName);
	SQL::CacheProcedureFromSql(Catalog, DbPath_, ProcedureName, std::move(BodyForCache), Logger_,
	                           SQL::OptimizationLevel::Advanced, this, true, OrReplace, std::move(SourceDialect),
	                           ExceptionHandlers);
	SQL::SaveProcedureCatalog(Catalog);
}

void Database::DropProcedureDefinition(const std::string &ProcedureName, bool IfExists) {
	{
		std::scoped_lock Guard(DbMutex_);
		auto It = ProcedureDefinitionSql_.find(ProcedureName);
		if(It == ProcedureDefinitionSql_.end()) {
			if(!IfExists)
				FailStorage("DROP PROCEDURE: \"" + ProcedureName + "\" does not exist.");
		} else {
			ProcedureDefinitionSql_.erase(It);
			AppendWalAfterDropProcedure(ProcedureName);
			Dirty_.store(true, std::memory_order_release);
		}
	}
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(DbPath_);
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	if(!SQL::UnregisterProcedure(Catalog, ProcedureName) && !IfExists)
		FailStorage("DROP PROCEDURE: \"" + ProcedureName + "\" does not exist.");
	SQL::SaveProcedureCatalog(Catalog);
}

void Database::ReplayWalDefineProcedure(const std::string &ProcedureName, std::string SqlBody) {
	{
		std::scoped_lock Guard(DbMutex_);
		if(ProcedureName.empty())
			FailStorage("WAL replay CREATE PROCEDURE: empty name.");
		ProcedureDefinitionSql_[ProcedureName] = SqlBody;
	}
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	SQL::CacheProcedureFromSql(Catalog, DbPath_, ProcedureName, std::move(SqlBody), Logger_, SQL::OptimizationLevel::Advanced,
	                           this, true);
	SQL::SaveProcedureCatalog(Catalog);
}

void Database::InstallTriggerScratchRowAssumeLocked(const std::string &TableName, const Item *Row) {
	if(!Row || Row->empty()) {
		Tables_.erase(TableName);
		TableSchemas_.erase(TableName);
		Indexes_.erase(TableName);
		return;
	}
	Schema Sch;
	Sch.reserve(Row->size());
	for(const auto &[Col, Val] : *Row) {
		Column C;
		C.Name = Col;
		C.DefaultValue = "TEXT";
		(void)Val;
		Sch.push_back(std::move(C));
	}
	TableSchemas_[TableName] = Sch;
	HybridTableSlot Slot;
	Slot.SetDeclaredPolicy(StorageLayout::Row);
	Slot.RowStore = {*Row};
	Slot.RecordWrite();
	Slot.SyncColumnarAfterRowMutation();
	Tables_[TableName] = std::move(Slot);
}

void Database::DefineTrigger(SQL::StoredTriggerEntry Spec, bool IfNotExists, bool OrReplace) {
	if(Spec.Name.empty())
		FailStorage("CREATE TRIGGER: name must be non-empty.");
	if(Spec.TableName.empty())
		FailStorage("CREATE TRIGGER: table name must be non-empty.");
	const auto SpecJson = SQL::SerializeTriggerRegistry({Spec});
	SQL::StoredTriggerEntry ForCache;
	{
		std::scoped_lock Guard(DbMutex_);
		if(Tables_.find(Spec.TableName) == Tables_.end() &&
		   ViewDefinitionSql_.find(Spec.TableName) == ViewDefinitionSql_.end())
			FailStorage("CREATE TRIGGER: table \"" + Spec.TableName + "\" does not exist.");
		const auto Existing = TriggerDefinitions_.find(Spec.Name);
		if(Existing != TriggerDefinitions_.end()) {
			if(IfNotExists)
				return;
			if(!OrReplace)
				FailStorage("CREATE TRIGGER: \"" + Spec.Name + "\" already exists.");
			TriggerDefinitions_.erase(Existing);
		}
		Spec.Enabled = true;
		TriggerDefinitions_[Spec.Name] = Spec;
		ForCache = TriggerDefinitions_[Spec.Name];
		AppendWalAfterDefineTrigger(Spec.Name, SpecJson);
		Dirty_.store(true, std::memory_order_release);
	}
	SQL::TriggerCatalog Catalog;
	const auto CatPath = SQL::DefaultTriggerCatalogPath(DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = SQL::LoadTriggerCatalog(CatPath);
	SQL::UnregisterTrigger(Catalog, Spec.Name);
	const SQL::StoredTriggerEntry Cached =
	    SQL::CacheTriggerFromSql(Catalog, DbPath_, std::move(ForCache), Logger_, SQL::OptimizationLevel::Advanced, this,
	                             true, OrReplace);
	{
		std::scoped_lock Guard(DbMutex_);
		TriggerDefinitions_[Spec.Name] = Cached;
	}
	SQL::SaveTriggerCatalog(Catalog);
}

void Database::DropTriggerDefinition(const std::string &TriggerName, bool IfExists) {
	{
		std::scoped_lock Guard(DbMutex_);
		auto It = TriggerDefinitions_.find(TriggerName);
		if(It == TriggerDefinitions_.end()) {
			if(!IfExists)
				FailStorage("DROP TRIGGER: \"" + TriggerName + "\" does not exist.");
		} else {
			TriggerDefinitions_.erase(It);
			AppendWalAfterDropTrigger(TriggerName);
			Dirty_.store(true, std::memory_order_release);
		}
	}
	SQL::TriggerCatalog Catalog;
	const auto CatPath = SQL::DefaultTriggerCatalogPath(DbPath_);
	Catalog = SQL::LoadTriggerCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	if(!SQL::UnregisterTrigger(Catalog, TriggerName) && !IfExists)
		FailStorage("DROP TRIGGER: \"" + TriggerName + "\" does not exist.");
	SQL::SaveTriggerCatalog(Catalog);
}

void Database::SetTriggerEnabled(const std::string &TriggerName, bool Enabled) {
	{
		std::scoped_lock Guard(DbMutex_);
		auto It = TriggerDefinitions_.find(TriggerName);
		if(It == TriggerDefinitions_.end())
			FailStorage("ALTER TRIGGER: \"" + TriggerName + "\" does not exist.");
		if(It->second.Enabled == Enabled)
			return;
		It->second.Enabled = Enabled;
		AppendWalAfterSetTriggerEnabled(TriggerName, Enabled);
		Dirty_.store(true, std::memory_order_release);
	}
	SQL::TriggerCatalog Catalog;
	const auto CatPath = SQL::DefaultTriggerCatalogPath(DbPath_);
	Catalog = SQL::LoadTriggerCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	for(SQL::StoredTriggerEntry &E : Catalog.Triggers) {
		if(E.Name == TriggerName) {
			E.Enabled = Enabled;
			break;
		}
	}
	SQL::SaveTriggerCatalog(Catalog);
}

void Database::ReplayWalDefineTrigger(const std::string &TriggerName, std::string SpecJson) {
	std::vector<SQL::StoredTriggerEntry> Entries;
	if(!SQL::DeserializeTriggerRegistry(SpecJson, Entries) || Entries.empty())
		FailStorage("WAL replay CREATE TRIGGER: corrupt spec for \"" + TriggerName + "\".");
	SQL::StoredTriggerEntry Spec = std::move(Entries.front());
	Spec.Name = TriggerName;
	{
		std::scoped_lock Guard(DbMutex_);
		TriggerDefinitions_[TriggerName] = Spec;
	}
	SQL::TriggerCatalog Catalog;
	const auto CatPath = SQL::DefaultTriggerCatalogPath(DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = SQL::LoadTriggerCatalog(CatPath);
	SQL::UnregisterTrigger(Catalog, TriggerName);
	SQL::CacheTriggerFromSql(Catalog, DbPath_, Spec, Logger_, SQL::OptimizationLevel::Advanced, this, true, true);
	SQL::SaveTriggerCatalog(Catalog);
}

void Database::ReplayWalDropTrigger(const std::string &TriggerName) {
	{
		std::scoped_lock Guard(DbMutex_);
		TriggerDefinitions_.erase(TriggerName);
	}
	SQL::TriggerCatalog Catalog;
	const auto CatPath = SQL::DefaultTriggerCatalogPath(DbPath_);
	Catalog = SQL::LoadTriggerCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	SQL::UnregisterTrigger(Catalog, TriggerName);
	SQL::SaveTriggerCatalog(Catalog);
}

void Database::ReplayWalSetTriggerEnabled(const std::string &TriggerName, bool Enabled) {
	{
		std::scoped_lock Guard(DbMutex_);
		auto It = TriggerDefinitions_.find(TriggerName);
		if(It != TriggerDefinitions_.end())
			It->second.Enabled = Enabled;
	}
	SQL::TriggerCatalog Catalog;
	const auto CatPath = SQL::DefaultTriggerCatalogPath(DbPath_);
	Catalog = SQL::LoadTriggerCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	for(SQL::StoredTriggerEntry &E : Catalog.Triggers) {
		if(E.Name == TriggerName) {
			E.Enabled = Enabled;
			break;
		}
	}
	SQL::SaveTriggerCatalog(Catalog);
}

void Database::ReplayWalDropProcedure(const std::string &ProcedureName) {
	{
		std::scoped_lock Guard(DbMutex_);
		ProcedureDefinitionSql_.erase(ProcedureName);
	}
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(DbPath_);
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	SQL::UnregisterProcedure(Catalog, ProcedureName);
	SQL::SaveProcedureCatalog(Catalog);
}

void Database::ReplayWalAddUser(std::string Name, std::string EncryptedBlob, std::array<uint8_t, 32> KeyMaterial) {
	std::scoped_lock Guard(DbMutex_);
	EncryptedString Es(std::move(EncryptedBlob), KeyMaterial);
	User Nu(std::move(Name), std::move(Es));
	for(auto &Existing : Users_) {
		if(Existing.Name == Nu.Name) {
			Existing = std::move(Nu);
			return;
		}
	}
	Users_.push_back(std::move(Nu));
}

void Database::ReplayWalRemoveUser(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	const auto It = std::find_if(Users_.begin(), Users_.end(), [&](const User &U) { return U.Name == Name; });
	if(It == Users_.end())
		return;
	if(CurrentUser_.has_value() && CurrentUser_->Name == Name)
		CurrentUser_.reset();
	Users_.erase(It);
}

void Database::ReplayWalGrantAcl(const std::string &UserName, const std::string &Table, int Bits) {
	std::scoped_lock Guard(DbMutex_);
	Acls_[UserName][Table] =
	    static_cast<Permissions>(static_cast<int>(Acls_[UserName][Table]) | Bits);
}

void Database::ReplayWalRevokeAcl(const std::string &UserName, const std::string &Table, int Bits) {
	std::scoped_lock Guard(DbMutex_);
	Acls_[UserName][Table] =
	    static_cast<Permissions>(static_cast<int>(Acls_[UserName][Table]) & ~Bits);
}

void Database::ReplayWalCreateRole(const std::string &RoleName) {
	std::scoped_lock Guard(DbMutex_);
	Roles_.insert(RoleName);
}

void Database::ReplayWalDropRole(const std::string &RoleName) {
	std::scoped_lock Guard(DbMutex_);
	Roles_.erase(RoleName);
	RoleAcls_.erase(RoleName);
	for(auto &[Un, Roles] : UserRoles_) {
		Roles.erase(std::remove(Roles.begin(), Roles.end(), RoleName), Roles.end());
	}
}

void Database::ReplayWalGrantRoleMembership(const std::string &RoleName, const std::string &UserName) {
	std::scoped_lock Guard(DbMutex_);
	Roles_.insert(RoleName);
	auto &Vec = UserRoles_[UserName];
	if(std::find(Vec.begin(), Vec.end(), RoleName) == Vec.end())
		Vec.push_back(RoleName);
}

void Database::ReplayWalRevokeRoleMembership(const std::string &RoleName, const std::string &UserName) {
	std::scoped_lock Guard(DbMutex_);
	auto &Vec = UserRoles_[UserName];
	Vec.erase(std::remove(Vec.begin(), Vec.end(), RoleName), Vec.end());
}

void Database::ReplayWalGrantRoleAcl(const std::string &RoleName, const std::string &Table, int Bits) {
	std::scoped_lock Guard(DbMutex_);
	Roles_.insert(RoleName);
	RoleAcls_[RoleName][Table] =
	    static_cast<Permissions>(static_cast<int>(RoleAcls_[RoleName][Table]) | Bits);
}

void Database::ReplayWalRevokeRoleAcl(const std::string &RoleName, const std::string &Table, int Bits) {
	std::scoped_lock Guard(DbMutex_);
	RoleAcls_[RoleName][Table] =
	    static_cast<Permissions>(static_cast<int>(RoleAcls_[RoleName][Table]) & ~Bits);
}

void Database::ReplayWalFineGrant(const std::string &UserName, RowColPermission Rule) {
	std::scoped_lock Guard(DbMutex_);
	for(User &U : Users_) {
		if(U.Name != UserName)
			continue;
		U.FineGrainedPermissions.push_back(std::move(Rule));
		return;
	}
}

void Database::ReplayWalFineRevoke(const std::string &UserName, RowColPermission Rule) {
	std::scoped_lock Guard(DbMutex_);
	for(User &U : Users_) {
		if(U.Name != UserName)
			continue;
		auto &Vec = U.FineGrainedPermissions;
		Vec.erase(std::remove_if(Vec.begin(), Vec.end(),
		                         [&](const RowColPermission &R) {
			                         return R.Table == Rule.Table && R.RowId == Rule.RowId &&
			                                R.Column == Rule.Column &&
			                                static_cast<int>(R.Perms) == static_cast<int>(Rule.Perms);
		                         }),
		          Vec.end());
		return;
	}
}

void Database::ReplayWalAddForeignKey(const std::string &TableName, ForeignKey Key) {
	std::scoped_lock Guard(DbMutex_);
	auto &Vec = ForeignKeys_[TableName];
	for(const ForeignKey &E : Vec) {
		if(E.ColumnName == Key.ColumnName && E.ReferencedTable == Key.ReferencedTable &&
		   E.ReferencedColumn == Key.ReferencedColumn && E.OnDelete == Key.OnDelete)
			return;
	}
	Vec.push_back(std::move(Key));
}

void Database::ReplaceTableLevelCheckConstraints(const std::string &TableName,
                                                 std::vector<std::pair<std::string, std::string>> Checks) {
	std::scoped_lock Guard(DbMutex_);
	if(Checks.empty())
		TableCheckConstraints_.erase(TableName);
	else
		TableCheckConstraints_[TableName] = std::move(Checks);
}

namespace {

static bool PermissionsInclude(Permissions Have, Permissions Need) {
	return (static_cast<int>(Have) & static_cast<int>(Need)) == static_cast<int>(Need);
}

static bool RuleMatchesRowKey(const RowColPermission &Rule, const std::string &Table, const std::string &RowKey) {
	if(Rule.Table != Table)
		return false;
	if(!Rule.RowId.empty() && Rule.RowId != RowKey)
		return false;
	return true;
}

static bool UserHasRowOnlyRulesForTable(const User &User, const std::string &Table) {
	for(const RowColPermission &Rule : User.FineGrainedPermissions) {
		if(Rule.Table == Table && Rule.Column.empty())
			return true;
	}
	return false;
}

static bool UserHasColumnRulesFor(const User &User, const std::string &Table, const std::string &Column) {
	for(const RowColPermission &Rule : User.FineGrainedPermissions) {
		if(Rule.Table == Table && Rule.Column == Column)
			return true;
	}
	return false;
}

static bool UserHasTableColumnRules(const User &User, const std::string &Table) {
	for(const RowColPermission &Rule : User.FineGrainedPermissions) {
		if(Rule.Table == Table && !Rule.Column.empty())
			return true;
	}
	return false;
}

} // namespace

bool Database::AclEnforcementActiveAssumeLocked() const {
	return CurrentUser_.has_value() && !CurrentUser_->Name.empty();
}

Permissions Database::EffectivePermissionsAssumeLocked(const User &User, const std::string &Table) const {
	int Bits = 0;
	const auto It = Acls_.find(User.Name);
	if(It != Acls_.end()) {
		const auto Glob = It->second.find("");
		if(Glob != It->second.end())
			Bits |= static_cast<int>(Glob->second);
		if(!Table.empty()) {
			const auto Tbl = It->second.find(Table);
			if(Tbl != It->second.end())
				Bits |= static_cast<int>(Tbl->second);
		}
	}
	const auto RolesIt = UserRoles_.find(User.Name);
	if(RolesIt != UserRoles_.end()) {
		for(const std::string &RoleName : RolesIt->second) {
			const auto RIt = RoleAcls_.find(RoleName);
			if(RIt == RoleAcls_.end())
				continue;
			const auto Glob = RIt->second.find("");
			if(Glob != RIt->second.end())
				Bits |= static_cast<int>(Glob->second);
			if(!Table.empty()) {
				const auto Tbl = RIt->second.find(Table);
				if(Tbl != RIt->second.end())
					Bits |= static_cast<int>(Tbl->second);
			}
		}
	}
	return static_cast<Permissions>(Bits);
}

void Database::EnsureBootstrapAdminAclAssumeLocked() {
	if(Acls_.find("Admin0") == Acls_.end())
		Acls_["Admin0"][""] = Permissions::All;
}

std::string Database::RowKeyAssumeLocked(const std::string &Table, const Item &Row) const {
	const auto SchIt = TableSchemas_.find(Table);
	if(SchIt == TableSchemas_.end())
		return {};
	for(const Column &Co : SchIt->second) {
		if(!Co.IsPrimaryKey)
			continue;
		const auto Cell = Row.find(Co.Name);
		if(Cell != Row.end())
			return Cell->second;
		return {};
	}
	if(Row.empty())
		return {};
	return Row.begin()->second;
}

bool Database::RowAllowsAssumeLocked(Permissions Perms, const std::string &Table, const Item &Row) const {
	if(!AclEnforcementActiveAssumeLocked())
		return true;
	const User &User = *CurrentUser_;
	if(!PermissionsInclude(EffectivePermissionsAssumeLocked(User, Table), Perms))
		return false;
	if(!UserHasRowOnlyRulesForTable(User, Table))
		return true;
	const std::string RowKey = RowKeyAssumeLocked(Table, Row);
	for(const RowColPermission &Rule : User.FineGrainedPermissions) {
		if(!Rule.Column.empty())
			continue;
		if(!RuleMatchesRowKey(Rule, Table, RowKey))
			continue;
		if(PermissionsInclude(Rule.Perms, Perms))
			return true;
	}
	return false;
}

bool Database::ColumnAllowsAssumeLocked(Permissions Perms, const std::string &Table, const Item &Row,
                                        const std::string &Column) const {
	if(!AclEnforcementActiveAssumeLocked())
		return true;
	const User &User = *CurrentUser_;
	if(!PermissionsInclude(EffectivePermissionsAssumeLocked(User, Table), Perms))
		return false;
	if(!UserHasColumnRulesFor(User, Table, Column)) {
		if(UserHasTableColumnRules(User, Table))
			return false;
		return RowAllowsAssumeLocked(Perms, Table, Row);
	}
	const std::string RowKey = RowKeyAssumeLocked(Table, Row);
	for(const RowColPermission &Rule : User.FineGrainedPermissions) {
		if(Rule.Table != Table || Rule.Column != Column)
			continue;
		if(!RuleMatchesRowKey(Rule, Table, RowKey))
			continue;
		if(PermissionsInclude(Rule.Perms, Perms))
			return true;
	}
	return false;
}

Database::Item Database::MaskRowForSelectAssumeLocked(const std::string &Table, const Item &Row) const {
	if(!AclEnforcementActiveAssumeLocked())
		return Row;
	Item Out;
	for(const auto &[ColumnName, Value] : Row) {
		if(ColumnAllowsAssumeLocked(Permissions::Select, Table, Row, ColumnName))
			Out[ColumnName] = Value;
	}
	return Out;
}

bool Database::IsRoleAssumeLocked(const std::string &Name) const {
	return Roles_.find(Name) != Roles_.end();
}

void Database::AuditRecordAssumeLocked(const std::string &Event, const std::string &Detail,
                                       const std::string &Outcome) const {
	std::string Actor;
	if(CurrentUser_.has_value())
		Actor = CurrentUser_->Name;
	AuditLog_.Record(Event, Actor, Detail, Outcome);
}

void Database::RequireSessionDdlAssumeLocked() const {
	if(!AclEnforcementActiveAssumeLocked())
		return;
	if(!PermissionsInclude(EffectivePermissionsAssumeLocked(*CurrentUser_, ""), Permissions::All)) {
		AuditRecordAssumeLocked("DDL_DENIED", "", "DENIED");
		FailStorage("DDL denied: ALL privilege required on the session user.");
	}
}

void Database::RequireSessionGrantAdminAssumeLocked() const {
	if(!AclEnforcementActiveAssumeLocked())
		return;
	if(!PermissionsInclude(EffectivePermissionsAssumeLocked(*CurrentUser_, ""), Permissions::All)) {
		AuditRecordAssumeLocked("GRANT_ADMIN_DENIED", "", "DENIED");
		FailStorage("GRANT/REVOKE denied: ALL privilege required on the session user.");
	}
}

void Database::RequireSessionTablePermissionAssumeLocked(Permissions Perms, const std::string &Table) const {
	if(!AclEnforcementActiveAssumeLocked())
		return;
	if(!PermissionsInclude(EffectivePermissionsAssumeLocked(*CurrentUser_, Table), Perms)) {
		AuditRecordAssumeLocked("TABLE_PERM_DENIED", Table, "DENIED");
		FailStorage("Permission denied on table \"" + Table + "\".");
	}
}

void Database::RequireSessionRowPermissionAssumeLocked(Permissions Perms, const std::string &Table,
                                                       const Item &Row) const {
	RequireSessionTablePermissionAssumeLocked(Perms, Table);
	if(!RowAllowsAssumeLocked(Perms, Table, Row)) {
		AuditRecordAssumeLocked("ROW_PERM_DENIED", Table, "DENIED");
		FailStorage("Permission denied for row in table \"" + Table + "\".");
	}
}

void Database::RequireSessionInsertAssumeLocked(const std::string &Table, const Item &Row) const {
	RequireSessionTablePermissionAssumeLocked(Permissions::Insert, Table);
	if(!RowAllowsAssumeLocked(Permissions::Insert, Table, Row))
		FailStorage("INSERT denied for row in table \"" + Table + "\".");
	for(const auto &[ColumnName, Value] : Row) {
		(void)Value;
		if(!ColumnAllowsAssumeLocked(Permissions::Insert, Table, Row, ColumnName))
			FailStorage("INSERT denied for column \"" + ColumnName + "\" on table \"" + Table + "\".");
	}
}

void Database::RequireSessionUpdateAssumeLocked(const std::string &Table, const Item &ExistingRow,
                                                const Item &NewValues) const {
	RequireSessionRowPermissionAssumeLocked(Permissions::Update, Table, ExistingRow);
	for(const auto &[ColumnName, Value] : NewValues) {
		(void)Value;
		Item Merged = ExistingRow;
		Merged[ColumnName] = NewValues.at(ColumnName);
		if(!ColumnAllowsAssumeLocked(Permissions::Update, Table, Merged, ColumnName))
			FailStorage("UPDATE denied for column \"" + ColumnName + "\" on table \"" + Table + "\".");
	}
}

static bool FkCellIsNull(const Database::Item &Row, const std::string &Col) {
	auto It = Row.find(Col);
	return It == Row.end() || It->second.empty();
}

static bool ParentHasFkValueAssumeLocked(const Database &Db, const std::string &ParentTable,
                                         const std::string &ParentCol, const std::string &Val) {
	auto Tit = Db.Tables_.find(ParentTable);
	if(Tit == Db.Tables_.end())
		return false;
	const HybridTableSlot::Table &ParentRows = Tit->second.RowStore;
	for(const auto &Pr : ParentRows) {
		auto Cit = Pr.find(ParentCol);
		if(Cit != Pr.end() && Cit->second == Val)
			return true;
	}
	return false;
}

static bool ParentHasCompositeFkTupleAssumeLocked(const Database &Db, const std::string &ParentTable,
                                                  const std::vector<ForeignKey> &Parts,
                                                  const Database::Item &Row) {
	auto Tit = Db.Tables_.find(ParentTable);
	if(Tit == Db.Tables_.end())
		return false;
	const HybridTableSlot::Table &ParentRows = Tit->second.RowStore;
	for(const auto &Pr : ParentRows) {
		bool Match = true;
		for(const ForeignKey &Fk : Parts) {
			auto CIt = Row.find(Fk.ColumnName);
			auto PIt = Pr.find(Fk.ReferencedColumn);
			if(CIt == Row.end() || PIt == Pr.end() || CIt->second != PIt->second) {
				Match = false;
				break;
			}
		}
		if(Match)
			return true;
	}
	return false;
}

static bool CompositeFkAnyChildColumnNull(const Database::Item &Row,
                                          const std::vector<ForeignKey> &Parts) {
	for(const ForeignKey &Fk : Parts)
		if(FkCellIsNull(Row, Fk.ColumnName))
			return true;
	return false;
}

void Database::RejectRowIfForeignKeysFailAssumeLocked(const std::string &TableName, const Item &Row) const {
	auto FkIt = ForeignKeys_.find(TableName);
	if(FkIt == ForeignKeys_.end())
		return;
	std::optional<std::string> RowPk;
	if(auto SchIt = TableSchemas_.find(TableName); SchIt != TableSchemas_.end()) {
		for(const Column &Co : SchIt->second) {
			if(!Co.IsPrimaryKey)
				continue;
			auto Pit = Row.find(Co.Name);
			if(Pit != Row.end() && !Pit->second.empty())
				RowPk = Pit->second;
			break;
		}
	}
	std::unordered_set<std::string> SeenGroups;
	for(const ForeignKey &Fk : FkIt->second) {
		if(!Fk.CompositeGroup.empty()) {
			if(!SeenGroups.insert(Fk.CompositeGroup).second)
				continue;
			std::vector<ForeignKey> Parts;
			Parts.reserve(FkIt->second.size());
			for(const ForeignKey &O : FkIt->second) {
				if(O.CompositeGroup == Fk.CompositeGroup)
					Parts.push_back(O);
			}
			if(CompositeFkAnyChildColumnNull(Row, Parts))
				continue;
			if(Parts[0].ReferencedTable == TableName && RowPk) {
				bool SelfRef = true;
				for(const ForeignKey &P : Parts) {
					auto It = Row.find(P.ColumnName);
					if(It == Row.end() || It->second != *RowPk) {
						SelfRef = false;
						break;
					}
				}
				if(SelfRef)
					continue;
			}
			if(ParentHasCompositeFkTupleAssumeLocked(*this, Parts[0].ReferencedTable, Parts, Row))
				continue;
			FailStorage("FOREIGN KEY violation on composite key in table \"" + TableName + "\".");
			continue;
		}
		if(FkCellIsNull(Row, Fk.ColumnName))
			continue;
		const std::string &Val = Row.at(Fk.ColumnName);
		if(Fk.ReferencedTable == TableName && RowPk && Val == *RowPk)
			continue;
		if(ParentHasFkValueAssumeLocked(*this, Fk.ReferencedTable, Fk.ReferencedColumn, Val))
			continue;
		FailStorage("FOREIGN KEY violation on \"" + TableName + "." + Fk.ColumnName +
		            "\": value \"" + Val + "\" is not present in \"" + Fk.ReferencedTable + "." +
		            Fk.ReferencedColumn + "\".");
	}
}

void Database::RejectDeleteIfReferencedAssumeLocked(const std::string &ParentTable, const Item &ParentRow) {
	std::vector<std::pair<std::string, size_t>> ChildRowsToDelete;
	std::vector<std::tuple<std::string, size_t, std::string>> ChildNullUpdates;
	for(const auto &[ChildTable, Fks] : ForeignKeys_) {
		std::unordered_set<std::string> ChildGroupsDone;
		for(const ForeignKey &Fk : Fks) {
			if(Fk.ReferencedTable != ParentTable)
				continue;
			if(!Fk.CompositeGroup.empty()) {
				if(!ChildGroupsDone.insert(Fk.CompositeGroup).second)
					continue;
				std::vector<ForeignKey> Parts;
				for(const ForeignKey &O : Fks) {
					if(O.CompositeGroup == Fk.CompositeGroup)
						Parts.push_back(O);
				}
				bool ParentNull = false;
				for(const ForeignKey &P : Parts) {
					if(FkCellIsNull(ParentRow, P.ReferencedColumn)) {
						ParentNull = true;
						break;
					}
				}
				if(ParentNull)
					continue;
				auto Cit = Tables_.find(ChildTable);
				if(Cit == Tables_.end())
					continue;
				for(size_t Ix = 0; Ix < Cit->second.RowStore.size(); ++Ix) {
					const Item &ChildRow = Cit->second.RowStore[Ix];
					if(CompositeFkAnyChildColumnNull(ChildRow, Parts))
						continue;
					bool Match = true;
					for(const ForeignKey &P : Parts) {
						if(ChildRow.at(P.ColumnName) != ParentRow.at(P.ReferencedColumn)) {
							Match = false;
							break;
						}
					}
					if(!Match)
						continue;
					if(Parts[0].OnDelete == ReferentialAction::Cascade)
						ChildRowsToDelete.emplace_back(ChildTable, Ix);
					else if(Parts[0].OnDelete == ReferentialAction::SetNull) {
						for(const ForeignKey &P : Parts)
							ChildNullUpdates.emplace_back(ChildTable, Ix, P.ColumnName);
					} else
						FailStorage("FOREIGN KEY violation: cannot delete referenced row in \"" +
						            ParentTable + "\" (referenced by \"" + ChildTable + "\").");
				}
				continue;
			}
			if(FkCellIsNull(ParentRow, Fk.ReferencedColumn))
				continue;
			const std::string &RefVal = ParentRow.at(Fk.ReferencedColumn);
			auto Cit = Tables_.find(ChildTable);
			if(Cit == Tables_.end())
				continue;
			for(size_t Ix = 0; Ix < Cit->second.RowStore.size(); ++Ix) {
				const Item &ChildRow = Cit->second.RowStore[Ix];
				if(FkCellIsNull(ChildRow, Fk.ColumnName))
					continue;
				if(ChildRow.at(Fk.ColumnName) != RefVal)
					continue;
				if(Fk.OnDelete == ReferentialAction::Cascade)
					ChildRowsToDelete.emplace_back(ChildTable, Ix);
				else if(Fk.OnDelete == ReferentialAction::SetNull)
					ChildNullUpdates.emplace_back(ChildTable, Ix, Fk.ColumnName);
				else
					FailStorage("FOREIGN KEY violation: cannot delete referenced row in \"" + ParentTable +
					            "\" (referenced by \"" + ChildTable + "." + Fk.ColumnName + "\").");
			}
		}
	}
	for(const auto &[Tbl, Ix, Col] : ChildNullUpdates)
		Tables_.at(Tbl).RowStore[Ix][Col] = std::string();
	std::sort(ChildRowsToDelete.begin(), ChildRowsToDelete.end(),
	          [](const auto &A, const auto &B) {
		          if(A.first != B.first)
			          return A.first > B.first;
		          return A.second > B.second;
	          });
	for(const auto &[Tbl, Ix] : ChildRowsToDelete) {
		Item Deleted = Tables_.at(Tbl).RowStore[Ix];
		Tables_.at(Tbl).RowStore.erase(Tables_.at(Tbl).RowStore.begin() + static_cast<std::ptrdiff_t>(Ix));
		Tables_.at(Tbl).SyncColumnarAfterRowMutation();
		RejectDeleteIfReferencedAssumeLocked(Tbl, Deleted);
	}
}

void Database::RejectRowIfChecksFailAssumeLocked(const std::string &TableName, const Item &Row) const {
	auto SchIt = TableSchemas_.find(TableName);
	if(SchIt == TableSchemas_.end())
		return;
	for(const Column &Co : SchIt->second) {
		if(!Co.CheckConstraintDnfPacked.has_value())
			continue;
		if(AstralDB::SQL::EvaluatePackedWhereDnf(this, Row, *Co.CheckConstraintDnfPacked))
			continue;
		const std::string Snip = Co.CheckConstraintSql.value_or(std::string());
		FailStorage("CHECK constraint failed on column \"" + Co.Name + "\"" +
		            (Snip.empty() ? std::string() : std::string(": ") + Snip));
	}
	auto TcIt = TableCheckConstraints_.find(TableName);
	if(TcIt == TableCheckConstraints_.end())
		return;
	for(const auto &Pr : TcIt->second) {
		if(AstralDB::SQL::EvaluatePackedWhereDnf(this, Row, Pr.second))
			continue;
		FailStorage("CHECK constraint failed (table-level): " + Pr.first);
	}
}

void Database::RejectRowIfTypesFailAssumeLocked(const std::string &TableName, const Item &Row) const {
	auto SchIt = TableSchemas_.find(TableName);
	if(SchIt == TableSchemas_.end())
		return;
	for(const Column &Co : SchIt->second) {
		if(!AdvancedTypes::IsAdvancedTypeSpelling(Co.DefaultValue))
			continue;
		const auto Desc = AdvancedTypes::ParseTypeSpelling(Co.DefaultValue);
		if(!Desc)
			continue;
		auto It = Row.find(Co.Name);
		if(It == Row.end() || It->second.empty())
			continue;
		if(!AdvancedTypes::ValidateCell(*Desc, It->second))
			FailStorage("Type validation failed for column \"" + Co.Name + "\" (" + Co.DefaultValue + ")");
	}
}

void Database::SetTableStoragePolicy(const std::string &TableName, StorageLayout Policy) {
	std::scoped_lock Guard(DbMutex_);
	RequireSessionDdlAssumeLocked();
	auto It = Tables_.find(TableName);
	if(It == Tables_.end())
		FailStorage("SET STORAGE: table \"" + TableName + "\" does not exist.");
	It->second.SetDeclaredPolicy(Policy);
	AppendWalAfterSetTableStorage(TableName, Policy);
	Dirty_.store(true, std::memory_order_release);
}

StorageLayout Database::TableStoragePolicy(const std::string &TableName) const {
	std::shared_lock Guard(DbMutex_);
	auto It = Tables_.find(TableName);
	if(It == Tables_.end())
		FailStorage("storage policy: table \"" + TableName + "\" does not exist.");
	return It->second.DeclaredPolicy;
}

void Database::ReplayWalSetTableStorage(const std::string &TableName, StorageLayout Policy) {
	std::scoped_lock Guard(DbMutex_);
	auto It = Tables_.find(TableName);
	if(It == Tables_.end())
		return;
	It->second.SetDeclaredPolicy(Policy);
}

std::future<void> Database::CreateTable(const std::string &TableName, const Schema &Columns,
                                        StorageLayout Storage) {
	Schema SchemaSnap(Columns);
	for(size_t Ix = 0; Ix < Columns.size(); ++Ix) {
		if(Columns[Ix].CheckConstraintDnfPacked.has_value() &&
		   !SchemaSnap[Ix].CheckConstraintDnfPacked.has_value())
			FailStorage("internal: CREATE TABLE snapshot dropped CHECK DNF for column \"" + Columns[Ix].Name +
			            "\".");
	}
	std::promise<void> Done;
	auto Fut = Done.get_future();
	AcquireAsyncBudget();
	try {
		{
			std::scoped_lock Guard(DbMutex_);
			RequireSessionDdlAssumeLocked();
			if(ViewDefinitionSql_.find(TableName) != ViewDefinitionSql_.end())
				FailStorage("CREATE TABLE: name \"" + TableName + "\" is already a VIEW.");
			if(TableSchemas_.find(TableName) != TableSchemas_.end())
				FailStorage("CREATE TABLE: table \"" + TableName + "\" already exists.");
			{
				const auto Emplaced = TableSchemas_.try_emplace(TableName, SchemaSnap);
				(void)Emplaced.first;
				if(!Emplaced.second)
					FailStorage("internal: CREATE TABLE try_emplace failed unexpectedly.");
			}
			for(size_t Ix = 0; Ix < Columns.size(); ++Ix) {
				if(Columns[Ix].CheckConstraintSql.has_value() &&
				   !TableSchemas_.at(TableName)[Ix].CheckConstraintSql.has_value())
					FailStorage("internal: CREATE TABLE lost CHECK SQL on column \"" + Columns[Ix].Name +
					            "\" after schema install.");
				if(Columns[Ix].CheckConstraintDnfPacked.has_value() &&
				   !TableSchemas_.at(TableName)[Ix].CheckConstraintDnfPacked.has_value())
					FailStorage("internal: CREATE TABLE lost CHECK DNF on column \"" + Columns[Ix].Name +
					            "\" after schema install.");
			}
			HybridTableSlot Slot;
			Slot.SetDeclaredPolicy(Storage);
			Tables_[TableName] = std::move(Slot);
			if(Storage != StorageLayout::Row)
				AppendWalAfterSetTableStorage(TableName, Storage);
			for(const Database::Column &Co : SchemaSnap) {
				if(!Co.IsIdentity || Co.IdentitySequenceName.empty())
					continue;
				if(Sequences_.count(Co.IdentitySequenceName) == 0) {
					SequenceState St;
					St.Start = Co.IdentityStart;
					St.Increment = Co.IdentityIncrement;
					St.Current = Co.IdentityStart;
					Sequences_[Co.IdentitySequenceName] = St;
					AppendWalAfterCreateSequence(Co.IdentitySequenceName, St.Start, St.Increment);
				}
			}
			AppendWalAfterCreate(TableName, SchemaSnap);
		}
		Dirty_.store(true, std::memory_order_release);
		Done.set_value();
	} catch(...) {
		ReleaseAsyncBudget();
		Done.set_exception(std::current_exception());
		return Fut;
	}
	ReleaseAsyncBudget();
	return Fut;
}

void Database::CollectFkDependentTablesAssumeLocked(const std::string &Root,
                                                    std::vector<std::string> &Out) const {
	for(const auto &[Child, Fks] : ForeignKeys_) {
		if(Child == Root)
			continue;
		for(const ForeignKey &Fk : Fks) {
			if(Fk.ReferencedTable != Root)
				continue;
			if(std::find(Out.begin(), Out.end(), Child) != Out.end())
				continue;
			Out.push_back(Child);
			CollectFkDependentTablesAssumeLocked(Child, Out);
			break;
		}
	}
}

std::future<void> Database::DropTable(const std::string &TableName, bool Cascade) {
	return DbDispatchAsync(this,[this, TableName, Cascade]() {
		{
			std::scoped_lock Guard(DbMutex_);
			RequireSessionDdlAssumeLocked();
			std::vector<std::string> Order;
			if(Cascade)
				CollectFkDependentTablesAssumeLocked(TableName, Order);
			Order.push_back(TableName);
			for(const std::string &Tn : Order) {
				if(TableSchemas_.find(Tn) == TableSchemas_.end())
					continue;
				AppendWalAfterDrop(Tn);
				TableSchemas_.erase(Tn);
				Tables_.erase(Tn);
				TypedTableBindings_.erase(Tn);
				Indexes_.erase(Tn);
				ForeignKeys_.erase(Tn);
				TableCheckConstraints_.erase(Tn);
			}
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::RenameColumn(const std::string &TableName, const std::string &FromColumn,
                                         const std::string &ToColumn) {
	return DbDispatchAsync(this,[this, TableName, FromColumn, ToColumn]() {
		{
			std::scoped_lock Guard(DbMutex_);
			RequireSessionDdlAssumeLocked();
			auto SchIt = TableSchemas_.find(TableName);
			if(SchIt == TableSchemas_.end())
				FailStorage("ALTER RENAME COLUMN: table \"" + TableName + "\" does not exist.");
			bool Found = false;
			for(Column &Co : SchIt->second) {
				if(Co.Name != FromColumn)
					continue;
				for(const Column &Other : SchIt->second)
					if(Other.Name == ToColumn)
						FailStorage("ALTER RENAME COLUMN: target \"" + ToColumn + "\" already exists.");
				Co.Name = ToColumn;
				Found = true;
				break;
			}
			if(!Found)
				FailStorage("ALTER RENAME COLUMN: column \"" + FromColumn + "\" not found.");
			for(Item &Row : Tables_.at(TableName).RowStore) {
				auto It = Row.find(FromColumn);
				if(It == Row.end())
					continue;
				Row[ToColumn] = std::move(It->second);
				Row.erase(FromColumn);
			}
			if(auto IdxOuter = Indexes_.find(TableName); IdxOuter != Indexes_.end()) {
				auto ColIt = IdxOuter->second.find(FromColumn);
				if(ColIt != IdxOuter->second.end()) {
					auto Node = IdxOuter->second.extract(ColIt);
					Node.key() = ToColumn;
					IdxOuter->second.insert(std::move(Node));
				}
			}
			for(auto &[Child, Fks] : ForeignKeys_) {
				for(ForeignKey &Fk : Fks) {
					if(Fk.ReferencedTable == TableName && Fk.ReferencedColumn == FromColumn)
						Fk.ReferencedColumn = ToColumn;
					if(Child == TableName && Fk.ColumnName == FromColumn)
						Fk.ColumnName = ToColumn;
				}
			}
			if(!WalSuspended_.load(std::memory_order_acquire))
				Wal_.AppendLine(std::string("RC|") + TableName + '|' + FromColumn + '|' + ToColumn);
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::AddColumn(const std::string &TableName, const Column &NewColumn) {
	return DbDispatchAsync(this,[this, TableName, NewColumn]() {
		{
			std::scoped_lock Guard(DbMutex_);
			RequireSessionDdlAssumeLocked();
			auto SchIt = TableSchemas_.find(TableName);
			if(SchIt == TableSchemas_.end())
				FailStorage("ALTER ADD COLUMN: table \"" + TableName + "\" does not exist.");
			for(const auto &C : SchIt->second)
				if(C.Name == NewColumn.Name)
					FailStorage("ALTER ADD COLUMN: column \"" + NewColumn.Name + "\" already exists on table \"" + TableName + "\".");
			SchIt->second.push_back(NewColumn);
			auto &RowStore = Tables_.at(TableName).RowStore;
			const std::string Fill =
			    NewColumn.InsertDefaultLiteral.has_value() ? *NewColumn.InsertDefaultLiteral : std::string();
			for(auto &Row : RowStore) {
				Row[NewColumn.Name] = Fill;
				RejectRowIfChecksFailAssumeLocked(TableName, Row);
			}
			auto IdxOuter = Indexes_.find(TableName);
			if(IdxOuter != Indexes_.end())
				IdxOuter->second.erase(NewColumn.Name);
			if(!WalSuspended_.load(std::memory_order_acquire)) {
				std::ostringstream O;
				O << "AC|" << TableName << '|' << NewColumn.Name << '|' << NewColumn.DefaultValue << '|'
				  << (NewColumn.IsPrimaryKey ? "1" : "0") << '|' << (NewColumn.IsUnique ? "1" : "0") << '|'
				  << (NewColumn.IsNotNull ? "1" : "0");
				Wal_.AppendLine(O.str());
			}
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::DropColumn(const std::string &TableName, const std::string &ColumnName) {
	return DbDispatchAsync(this,[this, TableName, ColumnName]() {
		{
			std::scoped_lock Guard(DbMutex_);
			RequireSessionDdlAssumeLocked();
			auto SchIt = TableSchemas_.find(TableName);
			if(SchIt == TableSchemas_.end())
				FailStorage("ALTER DROP COLUMN: table \"" + TableName + "\" does not exist.");
			auto &Sch = SchIt->second;
			const auto It =
			    std::remove_if(Sch.begin(), Sch.end(), [&](const Column &C) { return C.Name == ColumnName; });
			if(It == Sch.end())
				FailStorage("ALTER DROP COLUMN: column \"" + ColumnName + "\" not found on table \"" + TableName + "\".");
			Sch.erase(It, Sch.end());
			for(auto &Row : Tables_.at(TableName).RowStore)
				Row.erase(ColumnName);
			Tables_.at(TableName).SyncColumnarAfterRowMutation();
			auto IdxOuter = Indexes_.find(TableName);
			if(IdxOuter != Indexes_.end())
				IdxOuter->second.erase(ColumnName);
			auto FkIt = ForeignKeys_.find(TableName);
			if(FkIt != ForeignKeys_.end()) {
				auto &FkVec = FkIt->second;
				FkVec.erase(std::remove_if(FkVec.begin(), FkVec.end(),
				                              [&](const ForeignKey &Fk) { return Fk.ColumnName == ColumnName; }),
				             FkVec.end());
			}
			if(!WalSuspended_.load(std::memory_order_acquire))
				Wal_.AppendLine(std::string("DC|") + TableName + '|' + ColumnName);
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

namespace {

static constexpr const char *kNextValPrefix = "__astral_nextval__:";

} // namespace

void Database::ResolveRowSequenceLiteralsAssumeLocked(Item &Row) {
	for(auto &Pr : Row) {
		if(Pr.second.rfind(kNextValPrefix, 0) == 0) {
			const std::string Seq = Pr.second.substr(std::strlen(kNextValPrefix));
			Pr.second = NextSequenceValueAssumeLocked(Seq);
		}
	}
}

void Database::FillIdentityColumnsAssumeLocked(const Schema &Sch, Item &Row) {
	for(const Column &Co : Sch) {
		if(!Co.IsIdentity || Co.IdentitySequenceName.empty())
			continue;
		auto It = Row.find(Co.Name);
		const bool Missing =
		    It == Row.end() || It->second.empty() || It->second == "NULL";
		if(Missing) {
			Row[Co.Name] = NextSequenceValueAssumeLocked(Co.IdentitySequenceName);
			continue;
		}
		if(Co.IdentityAlways)
			FailStorage("INSERT: cannot supply a value for GENERATED ALWAYS AS IDENTITY column \"" + Co.Name +
			            "\".");
	}
}

std::future<void> Database::CreateSequence(const std::string &Name, int64_t Start, int64_t Increment,
                                           bool IfNotExists) {
	return DbDispatchAsync(this, [this, Name, Start, Increment, IfNotExists]() {
		{
			std::scoped_lock Guard(DbMutex_);
			RequireSessionDdlAssumeLocked();
			if(Sequences_.count(Name) != 0) {
				if(IfNotExists)
					return;
				FailStorage("CREATE SEQUENCE: sequence \"" + Name + "\" already exists.");
			}
			if(Increment == 0)
				FailStorage("CREATE SEQUENCE: INCREMENT BY must be non-zero.");
			SequenceState St;
			St.Start = Start;
			St.Increment = Increment;
			St.Current = Start;
			Sequences_[Name] = St;
			AppendWalAfterCreateSequence(Name, Start, Increment);
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::DropSequence(const std::string &Name, bool IfExists) {
	return DbDispatchAsync(this, [this, Name, IfExists]() {
		{
			std::scoped_lock Guard(DbMutex_);
			RequireSessionDdlAssumeLocked();
			if(Sequences_.erase(Name) == 0) {
				if(IfExists)
					return;
				FailStorage("DROP SEQUENCE: sequence \"" + Name + "\" does not exist.");
			}
			AppendWalAfterDropSequence(Name);
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

void Database::CreateObjectType(const std::string &TypeName, ObjectTypeSchema Fields) {
	std::scoped_lock Guard(DbMutex_);
	RequireSessionDdlAssumeLocked();
	if(TypeName.empty())
		FailStorage("CREATE TYPE requires a non-empty type name.");
	if(Fields.empty())
		FailStorage("CREATE TYPE requires at least one field.");
	if(ObjectTypes_.count(TypeName) != 0)
		FailStorage("CREATE TYPE: type \"" + TypeName + "\" already exists.");
	ObjectTypes_[TypeName] = std::move(Fields);
	AppendWalAfterCreateType(TypeName, ObjectTypes_.at(TypeName));
	Dirty_.store(true, std::memory_order_release);
}

void Database::DropObjectType(const std::string &TypeName, bool IfExists) {
	std::scoped_lock Guard(DbMutex_);
	RequireSessionDdlAssumeLocked();
	const auto ItType = ObjectTypes_.find(TypeName);
	if(ItType == ObjectTypes_.end()) {
		if(IfExists)
			return;
		FailStorage("DROP TYPE: type \"" + TypeName + "\" does not exist.");
	}
	for(const auto &Pr : TypedTableBindings_) {
		if(Pr.second == TypeName) {
			FailStorage("DROP TYPE: type \"" + TypeName + "\" is still referenced by typed table \"" + Pr.first +
			            "\".");
		}
	}
	ObjectTypes_.erase(ItType);
	AppendWalAfterDropType(TypeName);
	Dirty_.store(true, std::memory_order_release);
}

std::optional<Database::ObjectTypeSchema> Database::ResolveObjectTypeFields(const std::string &TypeName) const {
	std::shared_lock Guard(DbMutex_);
	auto It = ObjectTypes_.find(TypeName);
	if(It == ObjectTypes_.end())
		return std::nullopt;
	return It->second;
}

void Database::BindTypedTableToObjectType(const std::string &TableName, const std::string &TypeName) {
	std::scoped_lock Guard(DbMutex_);
	TypedTableBindings_[TableName] = TypeName;
	AppendWalAfterBindTypedTable(TableName, TypeName);
	Dirty_.store(true, std::memory_order_release);
}

std::string Database::NextSequenceValueAssumeLocked(const std::string &Name) {
	auto It = Sequences_.find(Name);
	if(It == Sequences_.end())
		FailStorage("NEXTVAL: sequence \"" + Name + "\" does not exist.");
	const int64_t Out = It->second.Current;
	It->second.Current += It->second.Increment;
	return std::to_string(Out);
}

std::string Database::NextSequenceValue(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	return NextSequenceValueAssumeLocked(Name);
}

void Database::ReplayWalCreateSequence(const std::string &Name, int64_t Start, int64_t Increment) {
	std::scoped_lock Guard(DbMutex_);
	SequenceState St;
	St.Start = Start;
	St.Increment = Increment;
	St.Current = Start;
	Sequences_[Name] = St;
}

void Database::ReplayWalDropSequence(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	Sequences_.erase(Name);
}

void Database::ReplayWalCreateType(const std::string &TypeName, ObjectTypeSchema Fields) {
	std::scoped_lock Guard(DbMutex_);
	ObjectTypes_[TypeName] = std::move(Fields);
}

void Database::ReplayWalDropType(const std::string &TypeName) {
	std::scoped_lock Guard(DbMutex_);
	ObjectTypes_.erase(TypeName);
	for(auto It = TypedTableBindings_.begin(); It != TypedTableBindings_.end();) {
		if(It->second == TypeName)
			It = TypedTableBindings_.erase(It);
		else
			++It;
	}
}

void Database::ReplayWalBindTypedTable(const std::string &TableName, const std::string &TypeName) {
	std::scoped_lock Guard(DbMutex_);
	TypedTableBindings_[TableName] = TypeName;
}

namespace {

bool IsEphemeralSessionDbPath(const std::filesystem::path &Path) {
	return Path.string().find("astraldb_session_") != std::string::npos;
}

std::string UpperAscii(std::string S) {
	for(char &C : S)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	return S;
}

void AssignBulkSyntheticCell(Database::Item &Row, const std::string &Col, int64_t RowId, std::size_t ColIndex,
                             std::size_t ColCount) {
	const std::string Key = UpperAscii(Col);
	if(Key == "ID")
		Row[Col] = std::to_string(RowId);
	else if(Key == "ACCT" || Key == "A")
		Row[Col] = std::to_string(RowId % 997);
	else if(Key == "AMOUNT" || (ColCount == 5 && ColIndex == 2))
		Row[Col] = std::to_string(RowId % 10000) + "." + std::to_string((RowId / 100) % 100);
	else if(Key == "TS" || Key == "TIMESTAMP" || (ColCount == 5 && ColIndex == 4))
		Row[Col] = std::to_string(1'704'067'200LL + RowId);
	else if(Key == "B" || (ColCount == 5 && ColIndex == 2))
		Row[Col] = std::string("txt_") + std::to_string(RowId);
	else if(Key == "C" || (ColCount == 5 && ColIndex == 3))
		Row[Col] = std::string("chk_") + std::to_string(RowId % 71);
	else if(Key == "D" || (ColCount == 5 && ColIndex == 4))
		Row[Col] = std::string("e_") + std::to_string(RowId / 17);
	else
		Row[Col] = std::to_string(RowId + static_cast<int64_t>(ColIndex));
}

} // namespace

void Database::InsertBulkSyntheticRows(const std::string &TableName, int64_t Count, int64_t StartId, int64_t Step) {
	if(Count <= 0)
		FailStorage("BULK INSERT row count must be a positive integer.");
	const bool Ephemeral = IsEphemeralSessionDbPath(DbPath_);
	const std::uint64_t Cap = Ephemeral ? Limits::MaxBulkInsertRowsBench : Limits::MaxBulkInsertRows;
	if(static_cast<std::uint64_t>(Count) > Cap)
		FailStorage("BULK INSERT row count exceeds the configured maximum (see server limits).");
	const bool FastPath = Ephemeral && static_cast<std::uint64_t>(Count) >= Limits::BulkFastPathMinRows;
	const std::size_t EstBytes = static_cast<std::size_t>(Count) * 256U;
	if(!MemoryGuard::SecureBoundsCheck(0, static_cast<std::size_t>(Count), 256U))
		FailStorage("BULK INSERT row count exceeds safe bounds.");
	if(!FastPath && !MemoryGuard::AllowAlloc(EstBytes))
		FailStorage("BULK INSERT would exceed the session memory guard (spike protection).");
	MemoryGuard::SpikeScope Spike;
	std::scoped_lock Guard(DbMutex_);
	const auto Tit = Tables_.find(TableName);
	if(Tit == Tables_.end())
		FailStorage("INSERT_BULK: table \"" + TableName + "\" does not exist.");
	const auto Sch = TableSchemaAssumeDbMutexHeld(TableName);
	if(!Sch.has_value() || Sch->empty())
		FailStorage("INSERT_BULK requires a non-empty table schema.");
	HybridTableSlot &Slot = Tit->second;
	Table &TableRef = Slot.RowStore;
	const size_t Begin = TableRef.size();
	if(Begin > TableRef.max_size() - static_cast<size_t>(Count))
		FailStorage("INSERT_BULK: row count would overflow table capacity.");
	TableRef.reserve(Begin + static_cast<size_t>(Count));
	std::vector<std::string> ColNames;
	ColNames.reserve(Sch->size());
	for(const Column &Co : *Sch)
		ColNames.push_back(Co.Name);
	const std::size_t ColCount = ColNames.size();
	if(FastPath) {
		constexpr int64_t BulkStreamChunkRows = 65'536;
		int64_t Remaining = Count;
		int64_t ChunkStart = StartId;
		while(Remaining > 0) {
			const int64_t ChunkRows = std::min<int64_t>(Remaining, BulkStreamChunkRows);
			const std::size_t ChunkEstimate = static_cast<std::size_t>(ChunkRows) * 96U;
			if(!MemoryGuard::AllowAlloc(ChunkEstimate))
				FailStorage("BULK INSERT fast path would exceed the session memory guard.");
			Superfetch::PrefetchAppendTarget(Slot.RowStore);
			AppendBulkSyntheticColumnar(Slot.Columnar, ColNames, ChunkRows, ChunkStart, Step);
			ChunkStart += ChunkRows * Step;
			Remaining -= ChunkRows;
		}
		Slot.ColumnarSynced = true;
		Slot.RecordWrite();
		Dirty_.store(true, std::memory_order_release);
		return;
	}
	const auto IdxOuter = Indexes_.find(TableName);
	for(int64_t K = 0; K < Count; ++K) {
		const int64_t RowId = StartId + K * Step;
		Item MutableRow;
		for(std::size_t Ci = 0; Ci < ColCount; ++Ci)
			AssignBulkSyntheticCell(MutableRow, ColNames[Ci], RowId, Ci, ColCount);
		if(!FastPath) {
			ResolveRowSequenceLiteralsAssumeLocked(MutableRow);
			FillIdentityColumnsAssumeLocked(*Sch, MutableRow);
			RequireSessionInsertAssumeLocked(TableName, MutableRow);
			RejectRowIfChecksFailAssumeLocked(TableName, MutableRow);
			RejectRowIfTypesFailAssumeLocked(TableName, MutableRow);
			RejectRowIfForeignKeysFailAssumeLocked(TableName, MutableRow);
		}
		TableRef.push_back(std::move(MutableRow));
		const size_t RowIndex = TableRef.size() - 1;
		const Item &Inserted = TableRef.back();
		if(!FastPath) {
			if(IdxOuter != Indexes_.end()) {
				for(const auto &[ColumnName, Value] : Inserted) {
					const auto ColIdx = IdxOuter->second.find(ColumnName);
					if(ColIdx != IdxOuter->second.end())
						std::get<BPlusTree<std::string, size_t>>(ColIdx->second.Index()).Insert(Value, RowIndex);
				}
			}
			AppendWalAfterInsert(TableName, Inserted);
			OnRowInsertedAssumeLocked(TableName, RowIndex, Inserted);
		}
	}
	Slot.RecordWrite();
	Slot.SyncColumnarAfterRowMutation();
	Dirty_.store(true, std::memory_order_release);
}

namespace {

int64_t NowEpochMs() {
	using Clock = std::chrono::system_clock;
	return static_cast<int64_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

const DatasetEntry *FindDatasetVersion(const DatasetCatalog &Cat, int64_t VersionId) {
	if(Cat.Versions.empty())
		return nullptr;
	if(VersionId <= 0)
		return &Cat.Versions.back();
	for(const DatasetEntry &E : Cat.Versions)
		if(E.VersionId == VersionId)
			return &E;
	return nullptr;
}

} // namespace

void Database::RegisterDataset(const std::string &Name, DatasetEntry Entry) {
	std::scoped_lock Guard(DbMutex_);
	if(Entry.Kind == DatasetKind::TableRef) {
		const auto SrcIt = Tables_.find(Entry.SourceTable);
		if(SrcIt == Tables_.end())
			FailStorage("CREATE DATASET: source table \"" + Entry.SourceTable + "\" does not exist.");
		const Table &Live = SrcIt->second.RowsForRead(std::nullopt, false);
		Entry.SnapshotRows.assign(Live.begin(), Live.end());
	}
	DatasetCatalog &Cat = Datasets_[Name];
	const int64_t NextVer = Cat.Versions.empty() ? 1 : Cat.Versions.back().VersionId + 1;
	Entry.VersionId = NextVer;
	Entry.CreatedAtMs = NowEpochMs();
	Cat.Versions.push_back(std::move(Entry));
}

void Database::DropDataset(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	Datasets_.erase(Name);
}

void Database::RegisterEmbedding(const std::string &Name, const std::string &TableName, const std::string &TokenColumn,
                                 const std::string &VectorColumn) {
	std::scoped_lock Guard(DbMutex_);
	EmbeddingCatalogEntry Entry = BuildEmbeddingCatalogEntryFromTable(*this, Name, TableName, TokenColumn, VectorColumn);
	InstallEmbeddingCatalogEntryAssumeLocked(*this, std::move(Entry));
	AppendWalAfterEmbeddingRegister(Embeddings_.at(Name));
	Dirty_.store(true, std::memory_order_release);
}

void Database::DropEmbedding(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	if(Embeddings_.find(Name) == Embeddings_.end())
		return;
	ReplayWalEmbeddingDropAssumeLocked(*this, Name);
	AppendWalAfterEmbeddingDrop(Name);
	Dirty_.store(true, std::memory_order_release);
}

std::optional<std::string> Database::EmbeddingWireCellAssumeDbMutexHeld(const std::string &Name) const {
	const auto It = Embeddings_.find(Name);
	if(It == Embeddings_.end())
		return std::nullopt;
	return It->second.WireCell;
}

std::optional<std::string> Database::EmbeddingWireCell(const std::string &Name) const {
	std::scoped_lock Guard(DbMutex_);
	return EmbeddingWireCellAssumeDbMutexHeld(Name);
}

void Database::AppendWalAfterGraphRegister(const GraphSpec &Spec) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(GraphWalLineRegister(Spec));
}

void Database::AppendWalAfterGraphDrop(const std::string &Name) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(GraphWalLineDrop(Name));
}

void Database::AppendWalAfterGraphProjection(const GraphProjectionRequest &Req) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(GraphWalLineProjection(Req));
}

void Database::AppendWalAfterEmbeddingRegister(const EmbeddingCatalogEntry &Entry) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(EmbeddingWalLineRegister(Entry));
}

void Database::AppendWalAfterEmbeddingDrop(const std::string &Name) {
	if(WalSuspended_.load(std::memory_order_acquire))
		return;
	Wal_.AppendLine(EmbeddingWalLineDrop(Name));
}

void Database::ReplayWalEmbeddingRegister(EmbeddingCatalogEntry Entry) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalEmbeddingRegisterAssumeLocked(*this, std::move(Entry));
}

void Database::ReplayWalEmbeddingDrop(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalEmbeddingDropAssumeLocked(*this, Name);
}

void Database::RegisterGraph(GraphSpec Spec) {
	const std::string Name = Spec.Name;
	std::scoped_lock Guard(DbMutex_);
	RegisterGraphCatalogEntryAssumeLocked(*this, std::move(Spec));
	AppendWalAfterGraphRegister(Graphs_.at(Name).first);
}

void Database::ReplayWalGraphRegister(GraphSpec Spec) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalGraphRegisterAssumeLocked(*this, std::move(Spec));
}

void Database::ReplayWalGraphDrop(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalGraphDropAssumeLocked(*this, Name);
}

void Database::ReplayWalGraphProjection(const GraphProjectionRequest &Req) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalGraphProjectionAssumeLocked(*this, Req);
}

void Database::DropGraph(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	if(Graphs_.find(Name) == Graphs_.end())
		return;
	ReplayWalGraphDropAssumeLocked(*this, Name);
	AppendWalAfterGraphDrop(Name);
}

const GraphSpec *Database::GraphByName(const std::string &Name) const {
	std::scoped_lock Guard(DbMutex_);
	const auto It = Graphs_.find(Name);
	if(It == Graphs_.end())
		return nullptr;
	return &It->second.first;
}

const GraphAdjacency *Database::GraphAdjacencyByName(const std::string &Name) const {
	std::scoped_lock Guard(DbMutex_);
	const auto It = Graphs_.find(Name);
	if(It == Graphs_.end())
		return nullptr;
	return &It->second.second;
}

void Database::GraphTraverse(const GraphTraverseRequest &Req) {
	GraphSpec Spec;
	GraphAdjacency Adj;
	{
		std::scoped_lock Guard(DbMutex_);
		const auto It = Graphs_.find(Req.GraphName);
		if(It == Graphs_.end())
			FailStorage("GRAPH \"" + Req.GraphName + "\" is not registered (CREATE GRAPH first).");
		Spec = It->second.first;
		Adj = It->second.second;
	}
	RunGraphTraverse(*this, Spec, Adj, Req);
}

void Database::ReplaceTableContents(const std::string &TableName, const Schema &Schema, Table Rows) {
	std::scoped_lock Guard(DbMutex_);
	if(TableSchemas_.find(TableName) != TableSchemas_.end()) {
		Tables_.erase(TableName);
		TableSchemas_.erase(TableName);
	}
	TableSchemas_[TableName] = Schema;
	HybridTableSlot Slot;
	Slot.SetDeclaredPolicy(StorageLayout::Row);
	Slot.RowStore = std::move(Rows);
	Slot.RecordWrite();
	Slot.SyncColumnarAfterRowMutation();
	Tables_[TableName] = std::move(Slot);
	Dirty_.store(true, std::memory_order_release);
}

void Database::RegisterGraphProjection(const GraphProjectionRequest &Req) {
	std::scoped_lock Guard(DbMutex_);
	const auto BaseIt = Graphs_.find(Req.BaseGraphName);
	if(BaseIt == Graphs_.end())
		FailStorage("CREATE GRAPH PROJECTION: base graph \"" + Req.BaseGraphName + "\" is not registered.");
	if(Graphs_.count(Req.ProjectionName))
		FailStorage("CREATE GRAPH PROJECTION: name \"" + Req.ProjectionName + "\" already exists.");
	GraphSpec Spec = BaseIt->second.first;
	Spec.Name = Req.ProjectionName;
	Spec.ProjectionOf = Req.BaseGraphName;
	Spec.ProjectionEdgeFilter = Req.EdgeLabelFilter;
	RegisterGraphCatalogEntryAssumeLocked(*this, std::move(Spec));
	AppendWalAfterGraphProjection(Req);
}

void Database::GraphMatch(const GraphMatchRequest &Req) {
	GraphSpec Spec;
	GraphAdjacency Adj;
	GraphEdgeTable Edges;
	{
		std::scoped_lock Guard(DbMutex_);
		const auto It = Graphs_.find(Req.GraphName);
		if(It == Graphs_.end())
			FailStorage("GRAPH \"" + Req.GraphName + "\" is not registered (CREATE GRAPH first).");
		Spec = It->second.first;
		Adj = It->second.second;
		const auto Vt = Tables_.find(Spec.VertexTable);
		if(Vt != Tables_.end()) {
			const Table &Live = Vt->second.RowsForRead(std::nullopt, false);
			Edges.assign(Live.begin(), Live.end());
		}
	}
	RunGraphMatch(*this, Spec, Adj, Req, Edges);
}

void Database::GraphShortestPath(const GraphShortestPathRequest &Req) {
	GraphSpec Spec;
	GraphAdjacency Adj;
	{
		std::scoped_lock Guard(DbMutex_);
		const auto It = Graphs_.find(Req.GraphName);
		if(It == Graphs_.end())
			FailStorage("GRAPH \"" + Req.GraphName + "\" is not registered (CREATE GRAPH first).");
		Spec = It->second.first;
		Adj = It->second.second;
	}
	RunGraphShortestPath(*this, Spec, Adj, Req);
}

void Database::GraphPageRank(const GraphPageRankRequest &Req) {
	GraphSpec Spec;
	GraphAdjacency Adj;
	GraphEdgeTable VertexRows;
	{
		std::scoped_lock Guard(DbMutex_);
		const auto It = Graphs_.find(Req.GraphName);
		if(It == Graphs_.end())
			FailStorage("GRAPH \"" + Req.GraphName + "\" is not registered (CREATE GRAPH first).");
		Spec = It->second.first;
		Adj = It->second.second;
		const auto Vt = Tables_.find(Spec.VertexTable);
		if(Vt != Tables_.end()) {
			const Table &Live = Vt->second.RowsForRead(std::nullopt, false);
			VertexRows.assign(Live.begin(), Live.end());
		}
	}
	RunGraphPageRank(*this, Spec, Adj, Req, VertexRows);
}

void Database::OnGraphEdgeInsertedAssumeLocked(const std::string &EdgeTable, const Item &Row) {
	const auto MapIt = GraphsByEdgeTable_.find(EdgeTable);
	if(MapIt == GraphsByEdgeTable_.end())
		return;
	for(const std::string &GraphName : MapIt->second) {
		const auto GIt = Graphs_.find(GraphName);
		if(GIt == Graphs_.end())
			continue;
		const GraphSpec &Spec = GIt->second.first;
		GraphAdjacencyInsertEdge(GIt->second.second, Spec, Row);
		for(const std::string &ProjName : GraphProjectionsByBase_[GraphName]) {
			const auto PIt = Graphs_.find(ProjName);
			if(PIt == Graphs_.end())
				continue;
			GraphAdjacencyInsertEdge(PIt->second.second, PIt->second.first, Row);
		}
	}
}

void Database::LoadDatasetInto(const std::string &Name, const std::string &TargetTable, const int64_t VersionId) {
	MemoryGuard::SpikeScope Spike;
	DatasetEntry Ent;
	{
		std::scoped_lock Guard(DbMutex_);
		const auto It = Datasets_.find(Name);
		if(It == Datasets_.end())
			FailStorage("DATASET \"" + Name + "\" is not registered.");
		if(Tables_.find(TargetTable) == Tables_.end())
			FailStorage("LOAD DATASET target table \"" + TargetTable + "\" does not exist.");
		const DatasetEntry *Found = FindDatasetVersion(It->second, VersionId);
		if(!Found)
			FailStorage("DATASET \"" + Name + "\" version " + std::to_string(VersionId) + " does not exist.");
		Ent = *Found;
	}
	if(Ent.Kind == DatasetKind::BulkFixture) {
		InsertBulkSyntheticRows(TargetTable, Ent.BulkCount, Ent.BulkStart, Ent.BulkStep);
		return;
	}
	std::scoped_lock Guard(DbMutex_);
	HybridTableSlot &Dst = Tables_[TargetTable];
	DatasetEntry::Table Src = Ent.SnapshotRows;
	if(Src.empty() && Ent.Kind == DatasetKind::TableRef) {
		const auto SrcIt = Tables_.find(Ent.SourceTable);
		if(SrcIt == Tables_.end())
			FailStorage("DATASET source table \"" + Ent.SourceTable + "\" does not exist.");
		const Table &Live = SrcIt->second.RowsForRead(std::nullopt, false);
		Src.assign(Live.begin(), Live.end());
	}
	if(Src.empty())
		FailStorage("DATASET \"" + Name + "\" has no rows to load.");
	const std::size_t EstBytes = Src.size() * 256U;
	if(!MemoryGuard::AllowAlloc(EstBytes))
		FailStorage("LOAD DATASET would exceed the session memory guard.");
	if(!MemoryGuard::SecureBoundsCheck(0, Src.size()))
		FailStorage("LOAD DATASET row count exceeds safe bounds.");
	Dst.RowStore.reserve(Dst.RowStore.size() + Src.size());
	const auto IdxOuter = Indexes_.find(TargetTable);
	for(const Item &Row : Src) {
		RequireSessionInsertAssumeLocked(TargetTable, Row);
		Dst.RowStore.push_back(Row);
		const size_t RowIndex = Dst.RowStore.size() - 1;
		const Item &Inserted = Dst.RowStore.back();
		if(IdxOuter != Indexes_.end()) {
			for(const auto &[ColumnName, Value] : Inserted) {
				const auto ColIdx = IdxOuter->second.find(ColumnName);
				if(ColIdx != IdxOuter->second.end())
					std::get<BPlusTree<std::string, size_t>>(ColIdx->second.Index()).Insert(Value, RowIndex);
			}
		}
		AppendWalAfterInsert(TargetTable, Inserted);
		OnRowInsertedAssumeLocked(TargetTable, RowIndex, Inserted);
	}
	Dst.RecordWrite();
	Dst.SyncColumnarAfterRowMutation();
	Dirty_.store(true, std::memory_order_release);
}

void Database::Vacuum(const std::string &TableName) {
	MemoryGuard::SpikeScope Spike;
	std::scoped_lock Guard(DbMutex_);
	const auto CompactOne = [](HybridTableSlot &Slot, const std::string &Name) {
		if(!MemoryGuard::AllowAlloc(Slot.RowStore.size() * 128U))
			FailStorage("VACUUM would exceed the session memory guard.");
		Slot.RowStore.shrink_to_fit();
		Slot.RebuildColumnarFromRows();
		Slot.SyncColumnarAfterRowMutation();
		Slot.RecordWrite();
		(void)Name;
	};
	if(TableName.empty()) {
		for(auto &[Name, Slot] : Tables_)
			CompactOne(Slot, Name);
	} else {
		const auto It = Tables_.find(TableName);
		if(It == Tables_.end())
			FailStorage("VACUUM: table \"" + TableName + "\" does not exist.");
		CompactOne(It->second, TableName);
	}
	SyncToFileUnlocked();
	MemoryGuard::ResetSession();
	Dirty_.store(true, std::memory_order_release);
}

void Database::RepackTableConcurrently(const std::string &TableName) {
	if(TableName.empty())
		FailStorage("REPACK: table name must be non-empty.");
	MemoryGuard::SpikeScope Spike;
	HybridTableSlot Shadow;
	std::vector<Database::Item> RowsCopy;
	Schema SchemaCopy;
	{
		std::scoped_lock Guard(DbMutex_);
		const auto TIt = Tables_.find(TableName);
		const auto SIt = TableSchemas_.find(TableName);
		if(TIt == Tables_.end() || SIt == TableSchemas_.end())
			FailStorage("REPACK: table \"" + TableName + "\" does not exist.");
		const std::size_t N = TIt->second.RowStore.size();
		const std::size_t EstBytes = N * 256U;
		if(!MemoryGuard::AllowAlloc(EstBytes))
			FailStorage("REPACK would exceed the session memory guard.");
		RowsCopy = TIt->second.RowStore;
		SchemaCopy = SIt->second;
	}
	Shadow.RowStore = std::move(RowsCopy);
	Shadow.RowStore.shrink_to_fit();
	Shadow.RebuildColumnarFromRows();
	Shadow.SyncColumnarAfterRowMutation();
	{
		std::scoped_lock Guard(DbMutex_);
		const auto TIt = Tables_.find(TableName);
		if(TIt == Tables_.end())
			FailStorage("REPACK: table \"" + TableName + "\" was dropped during repack.");
		TIt->second = std::move(Shadow);
		TIt->second.RecordWrite();
		const auto IdxOuter = Indexes_.find(TableName);
		if(IdxOuter != Indexes_.end()) {
			for(auto &[ColName, Idx] : IdxOuter->second)
				std::get<BPlusTree<std::string, size_t>>(Idx.Index()) = BPlusTree<std::string, size_t>{};
			for(size_t RowIndex = 0; RowIndex < TIt->second.RowStore.size(); ++RowIndex) {
				const Item &Row = TIt->second.RowStore[RowIndex];
				for(const auto &[ColumnName, Value] : Row) {
					const auto ColIdx = IdxOuter->second.find(ColumnName);
					if(ColIdx != IdxOuter->second.end())
						std::get<BPlusTree<std::string, size_t>>(ColIdx->second.Index()).Insert(Value, RowIndex);
				}
			}
		}
		for(size_t RowIndex = 0; RowIndex < TIt->second.RowStore.size(); ++RowIndex)
			OnRowInsertedAssumeLocked(TableName, RowIndex, TIt->second.RowStore[RowIndex]);
	}
	Dirty_.store(true, std::memory_order_release);
}

void Database::InsertRowAssumeLocked(const std::string &TableName, Item Row, bool RunTriggers) {
	if(Tables_.find(TableName) == Tables_.end())
		FailStorage("INSERT: table \"" + TableName + "\" does not exist.");
	Item MutableRow = std::move(Row);
	if(auto Sch = TableSchemaAssumeDbMutexHeld(TableName); Sch.has_value()) {
		ResolveRowSequenceLiteralsAssumeLocked(MutableRow);
		FillIdentityColumnsAssumeLocked(*Sch, MutableRow);
	}
	RequireSessionInsertAssumeLocked(TableName, MutableRow);
	RejectRowIfChecksFailAssumeLocked(TableName, MutableRow);
	RejectRowIfTypesFailAssumeLocked(TableName, MutableRow);
	RejectRowIfForeignKeysFailAssumeLocked(TableName, MutableRow);
	if(RunTriggers)
		FireTriggersAssumeLocked(*this, TableName, SQL::TriggerTiming::Before, SQL::TriggerEvent::Insert, nullptr,
		                         &MutableRow, true);
	HybridTableSlot &Slot = Tables_[TableName];
	Superfetch::PrefetchAppendTarget(Slot.RowStore);
	Slot.RowStore.push_back(MutableRow);
	Slot.RecordWrite();
	Slot.SyncColumnarAfterRowMutation();
	auto &TableRef = Slot.RowStore;
	auto IdxOuter = Indexes_.find(TableName);
	if(IdxOuter != Indexes_.end()) {
		for(const auto &[ColumnName, Value] : MutableRow) {
			auto ColIdx = IdxOuter->second.find(ColumnName);
			if(ColIdx != IdxOuter->second.end()) {
				auto &Index = ColIdx->second;
				std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(Value, TableRef.size() - 1);
			}
		}
	}
	AppendWalAfterInsert(TableName, MutableRow);
	OnRowInsertedAssumeLocked(TableName, TableRef.size() - 1, MutableRow);
	if(RunTriggers)
		FireTriggersAssumeLocked(*this, TableName, SQL::TriggerTiming::After, SQL::TriggerEvent::Insert, nullptr,
		                         &TableRef[TableRef.size() - 1], true);
}

std::future<void> Database::Insert(const std::string &TableName, const Item &Row) {
	auto DoInsert = [this, TableName, Row]() {
		if(DatabaseVmAssumeDbMutexHeld()) {
			InsertRowAssumeLocked(TableName, Row, true);
			Dirty_.store(true, std::memory_order_release);
			return;
		}
		{
			std::scoped_lock Guard(DbMutex_);
			InsertRowAssumeLocked(TableName, Row, true);
		}
		Dirty_.store(true, std::memory_order_release);
	};
	if(DatabaseVmAssumeDbMutexHeld()) {
		DoInsert();
		std::promise<void> Done;
		Done.set_value();
		return Done.get_future();
	}
	return DbDispatchAsync(this, [DoInsert = std::move(DoInsert)]() { DoInsert(); });
}

namespace {

bool ExistingRowMatchesInsertKeys(const Database::Item &Existing, const Database::Item &InsertRow,
                                  const std::vector<std::string> &Keys) {
	for(const std::string &K : Keys) {
		auto It = Existing.find(K);
		auto Jt = InsertRow.find(K);
		const std::string E = It != Existing.end() ? It->second : std::string();
		const std::string I = Jt != InsertRow.end() ? Jt->second : std::string();
		if(E != I)
			return false;
	}
	return true;
}

} // namespace

std::future<void> Database::Upsert(const std::string &TableName, const Database::Item &InsertRow,
                                   const std::vector<std::string> &ConflictColumnsIn, bool DoNothingOnConflict,
                                   const std::vector<std::pair<std::string, std::string>> &UpdateAssignments,
                                   bool SqliteReplace) {
	return DbDispatchAsync(this, [=, this]() {
		{
			std::scoped_lock Guard(DbMutex_);
			if(Tables_.find(TableName) == Tables_.end())
				FailStorage("UPSERT: table \"" + TableName + "\" does not exist.");
			std::vector<std::string> Keys = ConflictColumnsIn;
			if(Keys.empty()) {
				auto SchIt = TableSchemas_.find(TableName);
				if(SchIt == TableSchemas_.end())
					FailStorage("UPSERT: missing schema for \"" + TableName + "\".");
				for(const Column &Co : SchIt->second) {
					if(Co.IsPrimaryKey)
						Keys.push_back(Co.Name);
				}
				if(Keys.empty())
					FailStorage(
					    "UPSERT: table has no PRIMARY KEY; use ON CONFLICT (col,...) or define a PK on the table.");
			}
			for(const std::string &K : Keys) {
				if(InsertRow.find(K) == InsertRow.end())
					FailStorage("UPSERT: INSERT row must include conflict column \"" + K + "\".");
			}
			HybridTableSlot &Slot = Tables_.at(TableName);
			auto &TableRef = Slot.RowStore;
			size_t Found = static_cast<size_t>(-1);
			for(size_t I = 0; I < TableRef.size(); ++I) {
				if(ExistingRowMatchesInsertKeys(TableRef[I], InsertRow, Keys)) {
					Found = I;
					break;
				}
			}
			if(Found != static_cast<size_t>(-1)) {
				if(DoNothingOnConflict)
					return;
				Item &Ex = TableRef[Found];
				if(!RowAllowsAssumeLocked(Permissions::Update, TableName, Ex))
					FailStorage("UPSERT: UPDATE denied by ACL.");
				const SQL::RowEvalContext Ctx{&Ex, &InsertRow, nullptr};
				Item Merged = Ex;
				for(const auto &[Col, Blob] : UpdateAssignments) {
					auto V = SQL::EvalSerializedSetValueExpr(Blob, Ctx);
					if(!V)
						FailStorage("UPSERT: could not evaluate DO UPDATE SET for \"" + Col + "\".");
					Merged[Col] = *V;
				}
				for(const auto &[Col, Blob] : UpdateAssignments) {
					(void)Blob;
					if(!ColumnAllowsAssumeLocked(Permissions::Update, TableName, Merged, Col))
						FailStorage("UPSERT: column update denied for \"" + Col + "\".");
				}
				RejectRowIfChecksFailAssumeLocked(TableName, Merged);
				RejectRowIfForeignKeysFailAssumeLocked(TableName, Merged);
				auto IdxOuter = Indexes_.find(TableName);
				if(IdxOuter != Indexes_.end()) {
					for(const auto &[Col, Blob] : UpdateAssignments) {
						auto V = SQL::EvalSerializedSetValueExpr(Blob, Ctx);
						if(!V)
							continue;
						auto ColIdx = IdxOuter->second.find(Col);
						if(ColIdx == IdxOuter->second.end())
							continue;
						auto &Index = ColIdx->second;
						auto OldIt = Ex.find(Col);
						if(OldIt != Ex.end())
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Remove(OldIt->second);
						std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(*V, Found);
					}
				}
				for(const auto &[Col, Blob] : UpdateAssignments) {
					auto V = SQL::EvalSerializedSetValueExpr(Blob, Ctx);
					if(V)
						Ex[Col] = *V;
				}
				if(SqliteReplace) {
					auto SchIt = TableSchemas_.find(TableName);
					if(SchIt != TableSchemas_.end()) {
						for(const Column &Co : SchIt->second) {
							if(InsertRow.find(Co.Name) == InsertRow.end())
								Ex[Co.Name] = std::string();
						}
					}
				}
				Slot.RecordWrite();
				Slot.SyncColumnarAfterRowMutation();
			} else {
				RequireSessionInsertAssumeLocked(TableName, InsertRow);
				RejectRowIfChecksFailAssumeLocked(TableName, InsertRow);
				RejectRowIfForeignKeysFailAssumeLocked(TableName, InsertRow);
				Superfetch::PrefetchAppendTarget(TableRef);
				TableRef.push_back(InsertRow);
				auto IdxOuter = Indexes_.find(TableName);
				if(IdxOuter != Indexes_.end()) {
					for(const auto &[ColumnName, Value] : InsertRow) {
						auto ColIdx = IdxOuter->second.find(ColumnName);
						if(ColIdx != IdxOuter->second.end()) {
							auto &Index = ColIdx->second;
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(Value, TableRef.size() - 1);
						}
					}
				}
				Slot.RecordWrite();
				Slot.SyncColumnarAfterRowMutation();
				AppendWalAfterInsert(TableName, TableRef.back());
			}
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::MergeUsing(const std::string &TargetTable, const std::string &SourceTable,
                                       const std::vector<std::pair<std::string, std::string>> &KeyPairs,
                                       const std::vector<MergeUpdateCell> &OnMatch,
                                       const std::vector<MergeInsertCell> &OnInsert) {
	return DbDispatchAsync(this, [=, this]() {
		{
			std::scoped_lock Guard(DbMutex_);
			auto TIt = Tables_.find(TargetTable);
			auto SIt = Tables_.find(SourceTable);
			if(TIt == Tables_.end())
				FailStorage("MERGE: target table \"" + TargetTable + "\" does not exist.");
			if(SIt == Tables_.end())
				FailStorage("MERGE: source table \"" + SourceTable + "\" does not exist.");
			if(KeyPairs.empty())
				FailStorage("MERGE: at least one ON key pair is required.");
			auto &TRef = TIt->second.RowStore;
			auto &SRef = SIt->second.RowStore;
			for(const Item &SrcRow : SRef) {
				for(const auto &[TgtKey, SrcKey] : KeyPairs) {
					auto Sk = SrcRow.find(SrcKey);
					if(Sk == SrcRow.end())
						FailStorage("MERGE: source row missing join column \"" + SrcKey + "\".");
				}
				size_t Found = static_cast<size_t>(-1);
				for(size_t I = 0; I < TRef.size(); ++I) {
					bool AllMatch = true;
					for(const auto &[TgtKey, SrcKey] : KeyPairs) {
						auto Tk = TRef[I].find(TgtKey);
						auto Sk = SrcRow.find(SrcKey);
						if(Tk == TRef[I].end() || Sk == SrcRow.end() || Tk->second != Sk->second) {
							AllMatch = false;
							break;
						}
					}
					if(AllMatch) {
						Found = I;
						break;
					}
				}
				if(Found != static_cast<size_t>(-1)) {
					if(OnMatch.empty())
						continue;
					Item &Ex = TRef[Found];
					if(!RowAllowsAssumeLocked(Permissions::Update, TargetTable, Ex))
						continue;
					const SQL::RowEvalContext Ctx{&Ex, nullptr, &SrcRow};
					Item Merged = Ex;
					for(const MergeUpdateCell &M : OnMatch) {
						auto V = SQL::EvalSerializedSetValueExpr(M.ValueExpr, Ctx);
						if(!V)
							FailStorage("MERGE: could not evaluate UPDATE SET expression for \"" + M.TargetColumn +
							            "\".");
						Merged[M.TargetColumn] = *V;
					}
					bool ColumnOk = true;
					for(const MergeUpdateCell &M : OnMatch) {
						if(!ColumnAllowsAssumeLocked(Permissions::Update, TargetTable, Merged, M.TargetColumn)) {
							ColumnOk = false;
							break;
						}
					}
					if(!ColumnOk)
						continue;
					RejectRowIfChecksFailAssumeLocked(TargetTable, Merged);
					RejectRowIfForeignKeysFailAssumeLocked(TargetTable, Merged);
					auto IdxOuter = Indexes_.find(TargetTable);
					if(IdxOuter != Indexes_.end()) {
						for(const MergeUpdateCell &M : OnMatch) {
							auto V = SQL::EvalSerializedSetValueExpr(M.ValueExpr, Ctx);
							if(!V)
								continue;
							auto ColIdx = IdxOuter->second.find(M.TargetColumn);
							if(ColIdx == IdxOuter->second.end())
								continue;
							auto &Index = ColIdx->second;
							auto OldIt = Ex.find(M.TargetColumn);
							if(OldIt != Ex.end())
								std::get<BPlusTree<std::string, size_t>>(Index.Index()).Remove(OldIt->second);
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(*V, Found);
						}
					}
					for(const MergeUpdateCell &M : OnMatch) {
						auto V = SQL::EvalSerializedSetValueExpr(M.ValueExpr, Ctx);
						if(V)
							Ex[M.TargetColumn] = *V;
					}
				} else {
					if(OnInsert.empty())
						continue;
					Item NewR;
					const SQL::RowEvalContext Ctx{nullptr, nullptr, &SrcRow};
					for(const MergeInsertCell &N : OnInsert) {
						auto V = SQL::EvalSerializedSetValueExpr(N.ValueExpr, Ctx);
						if(!V)
							FailStorage("MERGE: could not evaluate INSERT value for \"" + N.Column + "\".");
						NewR[N.Column] = *V;
					}
					RequireSessionInsertAssumeLocked(TargetTable, NewR);
					RejectRowIfChecksFailAssumeLocked(TargetTable, NewR);
					RejectRowIfForeignKeysFailAssumeLocked(TargetTable, NewR);
					Superfetch::PrefetchAppendTarget(TRef);
					TRef.push_back(NewR);
					auto IdxOuter = Indexes_.find(TargetTable);
					if(IdxOuter != Indexes_.end()) {
						for(const auto &[ColumnName, Value] : NewR) {
							auto ColIdx = IdxOuter->second.find(ColumnName);
							if(ColIdx != IdxOuter->second.end()) {
								auto &Index = ColIdx->second;
								std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(Value, TRef.size() - 1);
							}
						}
					}
					AppendWalAfterInsert(TargetTable, TRef.back());
				}
			}
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::Delete(const std::string &TableName, const std::function<bool(const Item&)> &Condition) {
	return DbDispatchAsync(this,[this, TableName, Condition]() {
		{
			std::scoped_lock Guard(DbMutex_);
			RequireSessionTablePermissionAssumeLocked(Permissions::Delete, TableName);
			HybridTableSlot &Slot = Tables_.at(TableName);
			auto &TableRef = Slot.RowStore;
			auto IdxOuter = Indexes_.find(TableName);
			const auto RowPasses = [this, TableName](const Item &Row) {
				if(!RowAllowsAssumeLocked(Permissions::Delete, TableName, Row))
					return false;
				return true;
			};
			const auto DeleteCond = [&Condition, &RowPasses](const Item &Row) {
				return Condition(Row) && RowPasses(Row);
			};
			for(size_t i = 0; i < TableRef.size(); ++i) {
				if(DeleteCond(TableRef[i])) {
					FireTriggersAssumeLocked(*this, TableName, SQL::TriggerTiming::Before, SQL::TriggerEvent::Delete, &TableRef[i], nullptr, true);
					RejectDeleteIfReferencedAssumeLocked(TableName, TableRef[i]);
					if(IdxOuter != Indexes_.end()) {
						for(const auto& [ColumnName, Value] : TableRef[i]) {
							auto ColIdx = IdxOuter->second.find(ColumnName);
							if(ColIdx != IdxOuter->second.end()) {
								auto& Index = ColIdx->second;
								std::get<BPlusTree<std::string, size_t>>(Index.Index()).Remove(Value);
							}
						}
					}
					const Item DeletedRow = TableRef[i];
					FireTriggersAssumeLocked(*this, TableName, SQL::TriggerTiming::After, SQL::TriggerEvent::Delete,
					                         &DeletedRow, nullptr, true);
				}
			}
			TableRef.erase(std::remove_if(TableRef.begin(), TableRef.end(), DeleteCond), TableRef.end());
			SyncFtsIndexesForTableAssumeLocked(TableName);
			SyncVectorIndexesForTableAssumeLocked(TableName);
			Slot.RecordWrite();
			Slot.SyncColumnarAfterRowMutation();
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::UpdateWithSetExprs(const std::string &TableName,
                                             const std::function<bool(const Item &)> &Condition,
                                             const std::vector<std::pair<std::string, std::string>> &Assignments) {
	return DbDispatchAsync(this, [this, TableName, Condition, Assignments]() {
		bool Modified = false;
		{
			std::scoped_lock Guard(DbMutex_);
			auto TableIt = Tables_.find(TableName);
			if(TableIt == Tables_.end())
				FailStorage("UPDATE: table \"" + TableName + "\" does not exist.");
			HybridTableSlot &Slot = TableIt->second;
			auto &TableRef = Slot.RowStore;
			auto IdxOuter = Indexes_.find(TableName);
			RequireSessionTablePermissionAssumeLocked(Permissions::Update, TableName);
			for(size_t i = 0; i < TableRef.size(); ++i) {
				auto &Row = TableRef[i];
				if(!Condition(Row))
					continue;
				if(!RowAllowsAssumeLocked(Permissions::Update, TableName, Row))
					continue;
				const SQL::RowEvalContext Ctx{&Row, nullptr, nullptr};
				Item Merged = Row;
				for(const auto &[Col, Blob] : Assignments) {
					auto V = SQL::EvalSerializedSetValueExpr(Blob, Ctx);
					if(!V)
						FailStorage("UPDATE: could not evaluate SET expression for \"" + Col + "\".");
					Merged[Col] = *V;
				}
				bool ColumnOk = true;
				for(const auto &[Col, Blob] : Assignments) {
					(void)Blob;
					if(!ColumnAllowsAssumeLocked(Permissions::Update, TableName, Merged, Col)) {
						ColumnOk = false;
						break;
					}
				}
				if(!ColumnOk)
					continue;
				RejectRowIfChecksFailAssumeLocked(TableName, Merged);
				RejectRowIfForeignKeysFailAssumeLocked(TableName, Merged);
				if(IdxOuter != Indexes_.end()) {
					for(const auto &[Col, Blob] : Assignments) {
						auto V = SQL::EvalSerializedSetValueExpr(Blob, Ctx);
						if(!V)
							continue;
						auto ColIdx = IdxOuter->second.find(Col);
						if(ColIdx == IdxOuter->second.end())
							continue;
						auto &Index = ColIdx->second;
						auto OldIt = Row.find(Col);
						if(OldIt != Row.end())
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Remove(OldIt->second);
						std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(*V, i);
					}
				}
				Item OldRow = Row;
				for(const auto &[Col, Blob] : Assignments) {
					auto V = SQL::EvalSerializedSetValueExpr(Blob, Ctx);
					if(V)
						Row[Col] = *V;
				}
				OnRowUpdatedAssumeLocked(TableName, i, OldRow, Row);
				Modified = true;
			}
			if(Modified) {
				Slot.RecordWrite();
				Slot.SyncColumnarAfterRowMutation();
			}
		}
		if(Modified)
			Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::Update(const std::string &TableName, 
								   const std::function<bool(const Item&)> &Condition, 
								   const Item &NewValues) {
	return DbDispatchAsync(this,[this, TableName, Condition, NewValues]() {
		bool Modified = false;
		{
			std::scoped_lock Guard(DbMutex_);
			auto TableIt = Tables_.find(TableName);
			if(TableIt == Tables_.end())
				FailStorage("UPDATE: table \"" + TableName + "\" does not exist.");
			HybridTableSlot &Slot = TableIt->second;
			auto &TableRef = Slot.RowStore;
			auto IdxOuter = Indexes_.find(TableName);
			RequireSessionTablePermissionAssumeLocked(Permissions::Update, TableName);
			for(size_t i = 0; i < TableRef.size(); ++i) {
				auto& Row = TableRef[i];
				if(!Condition(Row))
					continue;
				if(!RowAllowsAssumeLocked(Permissions::Update, TableName, Row))
					continue;
				Item Merged = Row;
				for(const auto &Nv : NewValues)
					Merged[Nv.first] = Nv.second;
				bool ColumnOk = true;
				for(const auto &Nv : NewValues) {
					if(!ColumnAllowsAssumeLocked(Permissions::Update, TableName, Merged, Nv.first)) {
						ColumnOk = false;
						break;
					}
				}
				if(!ColumnOk)
					continue;
				RejectRowIfChecksFailAssumeLocked(TableName, Merged);
				RejectRowIfForeignKeysFailAssumeLocked(TableName, Merged);
				if(IdxOuter != Indexes_.end()) {
					for(const auto& [ColumnName, NewValue] : NewValues) {
						auto ColIdx = IdxOuter->second.find(ColumnName);
						if(ColIdx != IdxOuter->second.end()) {
							auto& Index = ColIdx->second;
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Remove(Row[ColumnName]);
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(NewValue, i);
						}
					}
				}
				Item OldRow = Row;
				FireTriggersAssumeLocked(*this, TableName, SQL::TriggerTiming::Before, SQL::TriggerEvent::Update, &Row, &Merged, true);
				for(const auto& [ColumnName, NewValue] : NewValues)
					Row[ColumnName] = NewValue;
				FireTriggersAssumeLocked(*this, TableName, SQL::TriggerTiming::After, SQL::TriggerEvent::Update, &OldRow, &Row, true);
				OnRowUpdatedAssumeLocked(TableName, i, OldRow, Row);
				Modified = true;
			}
			if(Modified) {
				Slot.RecordWrite();
				Slot.SyncColumnarAfterRowMutation();
			}
		}
		if(Modified)
			Dirty_.store(true, std::memory_order_release);
	});
}

std::future<Database::Table> Database::Select(const std::string &TableName, const std::function<bool(const Item&)> &Condition) const {
	return DbDispatchAsync(this,[this, TableName, Condition]() -> Table {
		Table Result;
		{
			std::shared_lock Guard(DbMutex_);
			auto TableIt = Tables_.find(TableName);
			if(TableIt == Tables_.end())
				FailStorage("SELECT: table \"" + TableName + "\" does not exist.");
			RequireSessionTablePermissionAssumeLocked(Permissions::Select, TableName);
			const HybridTableSlot &Slot = TableIt->second;
			const Table &TableRef = Slot.RowsForRead(std::nullopt, false);
			Superfetch::RowScanSession Scan;
			Superfetch::BeginRowScan(TableRef, Scan);
			auto IdxOuter = Indexes_.find(TableName);
			if(IdxOuter != Indexes_.end() && !IdxOuter->second.empty()) {
				for(const auto& ColumnIndexes : IdxOuter->second) {
					// Instead of iterating ColumnIndexes.second, get the BPlusTree and iterate its keys
					const auto& indexVariant = ColumnIndexes.second.Index();
					if (std::holds_alternative<BPlusTree<std::string, size_t>>(indexVariant)) {
						const auto& bptree = std::get<BPlusTree<std::string, size_t>>(indexVariant);
						for (const auto& key : bptree.GetAllKeys()) {
							size_t RowIndex;
							if (bptree.Search(key, RowIndex) && RowIndex < TableRef.size()) {
								const auto &Row = TableRef[RowIndex];
								if(Condition(Row) && RowAllowsAssumeLocked(Permissions::Select, TableName, Row))
									Result.push_back(MaskRowForSelectAssumeLocked(TableName, Row));
							}
						}
					}
				}
			} else if(!Scan.Armed) {
				for(const Item &Row : TableRef) {
					if(Condition(Row) && RowAllowsAssumeLocked(Permissions::Select, TableName, Row))
						Result.push_back(MaskRowForSelectAssumeLocked(TableName, Row));
				}
			} else {
				for(std::size_t Ri = 0; Ri < TableRef.size(); ++Ri) {
					Superfetch::AdvanceRowScan(TableRef, Ri, Scan);
					const Item &Row = TableRef[Ri];
					if(Condition(Row) && RowAllowsAssumeLocked(Permissions::Select, TableName, Row))
						Result.push_back(MaskRowForSelectAssumeLocked(TableName, Row));
				}
			}
		}
		return Result;
	});
}

std::future<bool> Database::ValidateRow(const std::string &TableName, const Item &Row) const {
	return DbDispatchAsync(this,[this, TableName, Row]() -> bool {
		bool Valid = true;
		{
			std::scoped_lock Guard(DbMutex_);
			auto SchemaIt = TableSchemas_.find(TableName);
			if(SchemaIt == TableSchemas_.end())
				return false;
			const Schema &Columns = SchemaIt->second;
			for(const auto &Column : Columns) {
				if((Column.IsPrimaryKey || Column.IsNotNull) && Row.find(Column.Name) == Row.end()) {
					Valid = false;
					break;
				}
				if(Column.IsUnique && Indexes_.find(TableName) != Indexes_.end()) {
					auto ItCol = Indexes_.at(TableName).find(Column.Name);
					if (ItCol != Indexes_.at(TableName).end() && Row.find(Column.Name) != Row.end()) {
						if (std::get<BPlusTree<std::string, size_t>>(ItCol->second.Index()).Contains(Row.at(Column.Name))) {
							Valid = false;
							break;
						}
					}
				}
			}
			if(Valid) {
				for(const auto &Co : Columns) {
					if(!Co.CheckConstraintDnfPacked.has_value())
						continue;
					if(AstralDB::SQL::EvaluatePackedWhereDnf(this, Row, *Co.CheckConstraintDnfPacked))
						continue;
					Valid = false;
					break;
				}
			}
			if(Valid) {
				auto Tk = TableCheckConstraints_.find(TableName);
				if(Tk != TableCheckConstraints_.end()) {
					for(const auto &Pr : Tk->second) {
						if(AstralDB::SQL::EvaluatePackedWhereDnf(this, Row, Pr.second))
							continue;
						Valid = false;
						break;
					}
				}
			}
		}
		return Valid;
	});
}

std::optional<Database::Schema> Database::TableSchemaSnapshot(const std::string &TableName) const {
	std::scoped_lock Guard(DbMutex_);
	auto It = TableSchemas_.find(TableName);
	if(It == TableSchemas_.end())
		return std::nullopt;
	return It->second;
}

std::optional<Database::Schema> Database::TableSchemaAssumeDbMutexHeld(const std::string &TableName) const {
	auto It = TableSchemas_.find(TableName);
	if(It == TableSchemas_.end())
		return std::nullopt;
	return It->second;
}

void Database::SetTableSchemaAssumeDbMutexHeld(const std::string &TableName, Schema Schema) {
	TableSchemas_[TableName] = std::move(Schema);
}

bool Database::LoadSnapshotFromDiskPathSynchronously(std::filesystem::path Path) {
	std::ifstream File(Path, std::ios::binary);
	if(!File) return false;
	std::string EncryptedData((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	std::string CompressedData = DecryptData(EncryptedData);
	std::string RawData = DecompressData(CompressedData);
	std::unordered_map<std::string, ObjectTypeSchema> LoadedObjectTypes;
	std::unordered_map<std::string, std::string> LoadedTypedBindings;
	if(!StripAndParseObjectTypeSnapshotTrailer(RawData, LoadedObjectTypes, LoadedTypedBindings))
		return false;
	std::vector<GraphSpec> LoadedGraphs;
	if(!StripAndParseGraphCatalogSnapshotTrailer(RawData, LoadedGraphs))
		return false;
	std::vector<EmbeddingCatalogEntry> LoadedEmbeddings;
	if(!StripAndParseEmbeddingCatalogSnapshotTrailer(RawData, LoadedEmbeddings))
		return false;
	std::unordered_map<std::string, std::string> LoadedViews;
	if(!StripAndParseViewSnapshotTrailer(RawData, LoadedViews))
		return false;
	std::unordered_map<std::string, SQL::StoredTriggerEntry> LoadedTriggers;
	if(!StripAndParseTriggerSnapshotTrailer(RawData, LoadedTriggers))
		return false;
	std::unordered_map<std::string, StorageSnapshotEntry> LoadedStorage;
	if(!StripAndParseStorageSnapshotTrailer(RawData, LoadedStorage))
		return false;
	std::unordered_map<std::string, SequenceState> LoadedSeqs;
	if(!StripAndParseSequenceSnapshotTrailer(RawData, LoadedSeqs))
		return false;
	std::unordered_map<std::string, std::vector<ForeignKey>> LoadedFks;
	if(!StripAndParseForeignKeySnapshotTrailer(RawData, LoadedFks))
		return false;
	bool ParsedUserAclPresent = false;
	std::vector<User> ParsedUsers;
	std::unordered_map<std::string, std::unordered_map<std::string, Permissions>> ParsedAcls;
	SecuritySnapshotExtras ParsedExtras;
	if(!StripAndParseUserAclSnapshotTrailer(RawData, ParsedUserAclPresent, ParsedUsers, ParsedAcls, ParsedExtras))
		return false;
	std::istringstream Input(RawData);
	size_t SchemaCount;
	Input >> SchemaCount;
	{
		std::scoped_lock Guard(DbMutex_);
		TableSchemas_.clear();
		Tables_.clear();
		ViewDefinitionSql_.clear();
		TriggerDefinitions_.clear();
		Indexes_.clear();
		FtsIndexes_.clear();
		VectorIndexes_.clear();
		ForeignKeys_.clear();
		TableCheckConstraints_.clear();
		Sequences_.clear();
		Graphs_.clear();
		GraphsByEdgeTable_.clear();
		GraphProjectionsByBase_.clear();
		Embeddings_.clear();
		Users_.clear();
		Acls_.clear();
		Roles_.clear();
		RoleAcls_.clear();
		UserRoles_.clear();
		ObjectTypes_.clear();
		TypedTableBindings_.clear();
		for(size_t i = 0; i < SchemaCount; ++i) {
			std::string TableName;
			Input >> TableName;
			size_t ColumnCount;
			Input >> ColumnCount;
			Schema NewSchema;
			for(size_t j = 0; j < ColumnCount; ++j) {
				Column NewColumn;
				Input >> NewColumn.Name >> NewColumn.IsPrimaryKey >> NewColumn.IsUnique >> NewColumn.IsNotNull
				    >> NewColumn.DefaultValue;
				TryReadTrailingConstraintColumns(Input, NewColumn);
				NewSchema.push_back(std::move(NewColumn));
			}
			TableSchemas_[TableName] = NewSchema;
		}
		size_t TableCount;
		Input >> TableCount;
		for(size_t i = 0; i < TableCount; ++i) {
			std::string TableName;
			Input >> TableName;
			size_t RowCount;
			Input >> RowCount;
			Table NewTable;
			for(size_t j = 0; j < RowCount; ++j) {
				size_t ItemCount;
				Input >> ItemCount;
				Item NewRow;
				for(size_t k = 0; k < ItemCount; ++k) {
					std::string Key, Value;
					Input >> Key >> Value;
					NewRow[Key] = Value;
				}
				NewTable.push_back(NewRow);
			}
			HybridTableSlot Slot;
			Slot.RowStore = std::move(NewTable);
			Tables_[TableName] = std::move(Slot);
		}
		for(const auto &[TName, Meta] : LoadedStorage) {
			auto It = Tables_.find(TName);
			if(It == Tables_.end())
				continue;
			It->second.SetDeclaredPolicy(Meta.Policy);
			if(!Meta.Weights.empty())
				It->second.Scheduler.DeserializeWeights(Meta.Weights);
		}
		ViewDefinitionSql_ = std::move(LoadedViews);
		TriggerDefinitions_ = std::move(LoadedTriggers);
		Sequences_ = std::move(LoadedSeqs);
		ForeignKeys_ = std::move(LoadedFks);
		if(ParsedUserAclPresent && !ParsedUsers.empty()) {
			Users_ = std::move(ParsedUsers);
			Acls_ = std::move(ParsedAcls);
			Roles_ = std::move(ParsedExtras.Roles);
			RoleAcls_ = std::move(ParsedExtras.RoleAcls);
			UserRoles_ = std::move(ParsedExtras.UserRoles);
			for(const auto &[Un, Rule] : ParsedExtras.FineGrants) {
				for(User &U : Users_) {
					if(U.Name == Un) {
						U.FineGrainedPermissions.push_back(Rule);
						break;
					}
				}
			}
		} else
			Users_.emplace_back("Admin0", "admin", Permissions::All);
		EnsureBootstrapAdminAclAssumeLocked();
		CurrentUser_.reset();
		InstallGraphCatalogAssumeLocked(*this, std::move(LoadedGraphs));
		InstallEmbeddingCatalogAssumeLocked(*this, std::move(LoadedEmbeddings));
		ObjectTypes_ = std::move(LoadedObjectTypes);
		TypedTableBindings_ = std::move(LoadedTypedBindings);
	}
	return true;
}

std::future<bool> Database::LoadFromFile(std::filesystem::path &Path) {
	std::filesystem::path Copy(Path);
	return DbDispatchAsync(this, [this, Copy = std::move(Copy)]() mutable -> bool {
		return LoadSnapshotFromDiskPathSynchronously(std::move(Copy));
	});
}

std::future<Database::Table> Database::JoinTables(const std::string &LeftTable, const std::string &RightTable,
								  const std::function<bool(const Item&, const Item&)> &JoinCondition) const {
	return DbDispatchAsync(this,[this, LeftTable, RightTable, JoinCondition]() -> Table {
		Table Result;
		{
			std::shared_lock Guard(DbMutex_);
			auto LeftIt = Tables_.find(LeftTable);
			auto RightIt = Tables_.find(RightTable);
			if(LeftIt == Tables_.end() || RightIt == Tables_.end())
				FailStorage("JOIN: one or both tables are missing (left=\"" + LeftTable + "\", right=\"" + RightTable +
				            "\").");
			const Table &LeftData = LeftIt->second.RowStore;
			const Table &RightData = RightIt->second.RowStore;
			Superfetch::JoinScanSession JoinScan;
			Superfetch::BeginJoinScan(LeftData, RightData, JoinScan);
			for(std::size_t Li = 0; Li < LeftData.size(); ++Li) {
				Superfetch::AdvanceJoinOuter(LeftData, Li, JoinScan);
				const Item &LeftRow = LeftData[Li];
				Superfetch::BeginInnerRescan(RightData, JoinScan.Inner);
				for(std::size_t Ri = 0; Ri < RightData.size(); ++Ri) {
					Superfetch::AdvanceJoinInner(RightData, Ri, JoinScan.Inner);
					const Item &RightRow = RightData[Ri];
					if(JoinCondition(LeftRow, RightRow)) {
						Item JoinedRow = RightRow;
						JoinedRow.insert(LeftRow.begin(), LeftRow.end());
						Result.push_back(JoinedRow);
					}
				}
			}
		}
		return Result;
	});
}

std::future<void> Database::AddForeignKey(const std::string &TableName, const ForeignKey &Key) {
	return DbDispatchAsync(this,[this, TableName, Key]() {
		std::scoped_lock Guard(DbMutex_);
		ForeignKeys_[TableName].push_back(Key);
		AppendWalAfterForeignKey(TableName, Key);
		Dirty_.store(true, std::memory_order_release);
	});
}

bool Database::HasPermission(const User &user, Permissions Perms, const std::string &Table) const {
	std::shared_lock Guard(DbMutex_);
	return PermissionsInclude(EffectivePermissionsAssumeLocked(user, Table), Perms);
}

std::future<void> Database::GrantPermission(const std::string &Username, Permissions Perms, const std::string &Table) {
	return DbDispatchAsync(this,[this, Username, Perms, Table]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		Acls_[Username][Table] = static_cast<Permissions>(static_cast<int>(Acls_[Username][Table]) | static_cast<int>(Perms));
		AppendWalAfterGrantAcl(Username, Table, static_cast<int>(Perms));
		AuditRecordAssumeLocked("GRANT_USER_ACL", Username + " ON " + Table, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::RevokePermission(const std::string &Username, Permissions Perms, const std::string &Table) {
	return DbDispatchAsync(this,[this, Username, Perms, Table]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		Acls_[Username][Table] = static_cast<Permissions>(static_cast<int>(Acls_[Username][Table]) & ~static_cast<int>(Perms));
		AppendWalAfterRevokeAcl(Username, Table, static_cast<int>(Perms));
		AuditRecordAssumeLocked("REVOKE_USER_ACL", Username + " ON " + Table, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<Permissions> Database::UserPermissions(const std::string &Username, const std::string &Table) const {
	return DbDispatchAsync(this,[this, Username, Table]() -> Permissions {
		std::shared_lock Guard(DbMutex_);
		for(const User &Registered : Users_) {
			if(Registered.Name == Username)
				return EffectivePermissionsAssumeLocked(Registered, Table);
		}
		User Placeholder(Username, EncryptedString());
		return EffectivePermissionsAssumeLocked(Placeholder, Table);
	});
}

std::future<void> Database::GrantRowPermission(const std::string &UserName, RowColPermission Rule) {
	return DbDispatchAsync(this, [this, UserName, Rule]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		bool Found = false;
		for(User &Registered : Users_) {
			if(Registered.Name != UserName)
				continue;
			Registered.FineGrainedPermissions.push_back(Rule);
			Found = true;
			if(CurrentUser_.has_value() && CurrentUser_->Name == UserName)
				CurrentUser_.emplace(Registered);
			break;
		}
		if(!Found)
			FailStorage("GrantRowPermission: user \"" + UserName + "\" not found.");
		AppendWalAfterFineGrant(UserName, Rule);
		AuditRecordAssumeLocked("GRANT_FINE", UserName + " " + Rule.Table + "/" + Rule.Column, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::RevokeRowPermission(const std::string &UserName, RowColPermission Rule) {
	return DbDispatchAsync(this, [this, UserName, Rule]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		bool Found = false;
		for(User &Registered : Users_) {
			if(Registered.Name != UserName)
				continue;
			auto &Vec = Registered.FineGrainedPermissions;
			const size_t Before = Vec.size();
			Vec.erase(std::remove_if(Vec.begin(), Vec.end(),
			                         [&](const RowColPermission &R) {
				                         return R.Table == Rule.Table && R.RowId == Rule.RowId &&
				                                R.Column == Rule.Column &&
				                                static_cast<int>(R.Perms) == static_cast<int>(Rule.Perms);
			                         }),
			          Vec.end());
			Found = Before != Vec.size();
			if(CurrentUser_.has_value() && CurrentUser_->Name == UserName)
				CurrentUser_.emplace(Registered);
			break;
		}
		if(!Found)
			FailStorage("RevokeRowPermission: matching rule not found for user \"" + UserName + "\".");
		AppendWalAfterFineRevoke(UserName, Rule);
		AuditRecordAssumeLocked("REVOKE_FINE", UserName + " " + Rule.Table + "/" + Rule.Column, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::CreateRole(const std::string &RoleName) {
	return DbDispatchAsync(this, [this, RoleName]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		if(RoleName.empty())
			FailStorage("CREATE ROLE: name must be non-empty.");
		if(Roles_.count(RoleName))
			FailStorage("CREATE ROLE: role \"" + RoleName + "\" already exists.");
		Roles_.insert(RoleName);
		AppendWalAfterCreateRole(RoleName);
		AuditRecordAssumeLocked("CREATE_ROLE", RoleName, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::DropRole(const std::string &RoleName) {
	return DbDispatchAsync(this, [this, RoleName]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		if(!Roles_.count(RoleName))
			FailStorage("DROP ROLE: role \"" + RoleName + "\" does not exist.");
		Roles_.erase(RoleName);
		RoleAcls_.erase(RoleName);
		for(auto &[Un, Roles] : UserRoles_) {
			Roles.erase(std::remove(Roles.begin(), Roles.end(), RoleName), Roles.end());
		}
		AppendWalAfterDropRole(RoleName);
		AuditRecordAssumeLocked("DROP_ROLE", RoleName, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::GrantRoleToUser(const std::string &RoleName, const std::string &UserName) {
	return DbDispatchAsync(this, [this, RoleName, UserName]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		if(!Roles_.count(RoleName))
			FailStorage("GRANT ROLE: role \"" + RoleName + "\" does not exist.");
		bool UserExists = false;
		for(const User &U : Users_) {
			if(U.Name == UserName) {
				UserExists = true;
				break;
			}
		}
		if(!UserExists)
			FailStorage("GRANT ROLE: user \"" + UserName + "\" not found.");
		auto &Vec = UserRoles_[UserName];
		if(std::find(Vec.begin(), Vec.end(), RoleName) == Vec.end())
			Vec.push_back(RoleName);
		AppendWalAfterGrantRoleMembership(RoleName, UserName);
		AuditRecordAssumeLocked("GRANT_ROLE_MEMBERSHIP", RoleName + " TO " + UserName, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::RevokeRoleFromUser(const std::string &RoleName, const std::string &UserName) {
	return DbDispatchAsync(this, [this, RoleName, UserName]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		auto &Vec = UserRoles_[UserName];
		Vec.erase(std::remove(Vec.begin(), Vec.end(), RoleName), Vec.end());
		AppendWalAfterRevokeRoleMembership(RoleName, UserName);
		AuditRecordAssumeLocked("REVOKE_ROLE_MEMBERSHIP", RoleName + " FROM " + UserName, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::GrantRolePermission(const std::string &RoleName, Permissions Perms,
                                                const std::string &Table) {
	return DbDispatchAsync(this, [this, RoleName, Perms, Table]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		if(!Roles_.count(RoleName))
			FailStorage("GRANT on ROLE: role \"" + RoleName + "\" does not exist.");
		RoleAcls_[RoleName][Table] =
		    static_cast<Permissions>(static_cast<int>(RoleAcls_[RoleName][Table]) | static_cast<int>(Perms));
		AppendWalAfterGrantRoleAcl(RoleName, Table, static_cast<int>(Perms));
		AuditRecordAssumeLocked("GRANT_ROLE_ACL", RoleName + " ON " + Table, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::RevokeRolePermission(const std::string &RoleName, Permissions Perms,
                                                 const std::string &Table) {
	return DbDispatchAsync(this, [this, RoleName, Perms, Table]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		RoleAcls_[RoleName][Table] =
		    static_cast<Permissions>(static_cast<int>(RoleAcls_[RoleName][Table]) & ~static_cast<int>(Perms));
		AppendWalAfterRevokeRoleAcl(RoleName, Table, static_cast<int>(Perms));
		AuditRecordAssumeLocked("REVOKE_ROLE_ACL", RoleName + " ON " + Table, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

void Database::SetAuditLogPath(std::optional<std::filesystem::path> Path) {
	std::scoped_lock Guard(DbMutex_);
	AuditLog_.SetPath(std::move(Path));
}

bool Database::AuthenticateUser(const std::string& Username, const std::string& Password) {
	std::scoped_lock Guard(DbMutex_);
	for(const auto& Registered : Users_) {
		if(Registered.Name == Username && Registered.VerifyPassword(Password)) {
			CurrentUser_.emplace(Registered);
			AuditRecordAssumeLocked("LOGIN", Username, "OK");
			return true;
		}
	}
	AuditRecordAssumeLocked("LOGIN", Username, "DENIED");
	return false;
}

std::future<void> Database::AddUser(const User& NewUser) {
	return DbDispatchAsync(this, [this, NewUser]() {
		std::scoped_lock Guard(DbMutex_);
		for(const auto& Existing : Users_) {
			if(Existing.Name == NewUser.Name)
				FailStorage("AddUser: user \"" + NewUser.Name + "\" already exists.");
		}
		Users_.push_back(NewUser);
		AppendWalAfterAddUser(Users_.back());
		AuditRecordAssumeLocked("ADD_USER", NewUser.Name, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::RemoveUser(const User& UserRef) {
	return DbDispatchAsync(this, [this, Name = UserRef.Name]() {
		std::scoped_lock Guard(DbMutex_);
		const auto It = std::find_if(Users_.begin(), Users_.end(),
		                             [&](const User& U) { return U.Name == Name; });
		if(It == Users_.end())
			FailStorage("RemoveUser: user \"" + Name + "\" not found.");
		if(CurrentUser_.has_value() && CurrentUser_->Name == Name)
			CurrentUser_.reset();
		AppendWalAfterRemoveUser(Name);
		Users_.erase(It);
		UserRoles_.erase(Name);
		AuditRecordAssumeLocked("REMOVE_USER", Name, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::CreateUser(const std::string &UserName, const std::string &Password, bool IfNotExists) {
	return DbDispatchAsync(this, [this, UserName, Password, IfNotExists]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		if(UserName.empty())
			FailStorage("CREATE USER: name must be non-empty.");
		for(const auto &Existing : Users_) {
			if(Existing.Name == UserName) {
				if(IfNotExists)
					return;
				FailStorage("CREATE USER: user \"" + UserName + "\" already exists.");
			}
		}
		Users_.emplace_back(UserName, Password, static_cast<Permissions>(0));
		AppendWalAfterAddUser(Users_.back());
		AuditRecordAssumeLocked("CREATE_USER", UserName, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::DropUser(const std::string &UserName, bool IfExists) {
	return DbDispatchAsync(this, [this, UserName, IfExists]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		const auto It = std::find_if(Users_.begin(), Users_.end(),
		                             [&](const User &U) { return U.Name == UserName; });
		if(It == Users_.end()) {
			if(IfExists)
				return;
			FailStorage("DROP USER: user \"" + UserName + "\" not found.");
		}
		if(CurrentUser_.has_value() && CurrentUser_->Name == UserName)
			CurrentUser_.reset();
		AppendWalAfterRemoveUser(UserName);
		Users_.erase(It);
		UserRoles_.erase(UserName);
		Acls_.erase(UserName);
		AuditRecordAssumeLocked("DROP_USER", UserName, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::AlterUserPassword(const std::string &UserName, const std::string &NewPassword) {
	return DbDispatchAsync(this, [this, UserName, NewPassword]() {
		std::scoped_lock Guard(DbMutex_);
		RequireSessionGrantAdminAssumeLocked();
		const auto It = std::find_if(Users_.begin(), Users_.end(),
		                             [&](const User &U) { return U.Name == UserName; });
		if(It == Users_.end())
			FailStorage("ALTER USER: user \"" + UserName + "\" not found.");
		const User Updated(UserName, NewPassword, static_cast<Permissions>(0));
		It->Password = Updated.Password;
		AppendWalAfterAddUser(*It);
		if(CurrentUser_.has_value() && CurrentUser_->Name == UserName)
			CurrentUser_->Password = It->Password;
		AuditRecordAssumeLocked("ALTER_USER_PASSWORD", UserName, "OK");
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::SetCurrentUser(const User& UserRef) {
	return DbDispatchAsync(this, [this, Name = UserRef.Name]() {
		std::scoped_lock Guard(DbMutex_);
		for(const auto& Registered : Users_) {
			if(Registered.Name == Name) {
				CurrentUser_.emplace(Registered);
				return;
			}
		}
		FailStorage("SetCurrentUser: user \"" + Name + "\" is not registered.");
	});
}

void Database::Logout() {
	std::scoped_lock Guard(DbMutex_);
	if(CurrentUser_.has_value())
		AuditRecordAssumeLocked("LOGOUT", CurrentUser_->Name, "OK");
	CurrentUser_.reset();
}

bool Database::IsAuthenticated() const {
	std::shared_lock Guard(DbMutex_);
	return CurrentUser_.has_value() && !CurrentUser_->Name.empty();
}

const std::optional<User>& Database::CurrentUser() const {
	return CurrentUser_;
}

// Helper: get or create index for a table/column
IndexManagement<std::string, size_t>& Database::GetOrCreateIndex(const std::string& table, const std::string& column) {
	if (Indexes_[table].find(column) == Indexes_[table].end()) {
		Indexes_[table].emplace(std::piecewise_construct,
			std::forward_as_tuple(column),
			std::forward_as_tuple());
	}
	return Indexes_[table][column];
}

std::future<void> Database::AddIndex(const std::string &TableName, const std::string &ColumnName) {
	return DbDispatchAsync(this,[this, TableName, ColumnName]() {
		std::scoped_lock Guard(DbMutex_);
		auto& table = Tables_[TableName].RowStore;
		auto& index = GetOrCreateIndex(TableName, ColumnName);
		for (size_t i = 0; i < table.size(); ++i) {
			const auto& row = table[i];
			auto it = row.find(ColumnName);
			if (it != row.end()) {
				// For now, always use BPlusTree
				std::get<BPlusTree<std::string, size_t>>(index.Index()).Insert(it->second, i);
			}
		}
	});
}

std::future<void> Database::RemoveIndex(const std::string &TableName, const std::string &ColumnName) {
	return DbDispatchAsync(this,[this, TableName, ColumnName]() {
		std::scoped_lock Guard(DbMutex_);
		Indexes_[TableName].erase(ColumnName);
	});
}

bool Database::ExportBundle(std::filesystem::path Destination, std::string_view FormatIn) {
	try {
		const std::string Fmt = NormFmt(FormatIn);
		std::ofstream Out(Destination, std::ios::binary);
		if(!Out) {
			if(Logger_)
				Logger_->Error("Failed to open export file");
			return false;
		}
		std::vector<std::string> Names;
		{
			std::shared_lock Guard(DbMutex_);
			Names.reserve(TableSchemas_.size());
			for(const auto &P : TableSchemas_)
				Names.push_back(P.first);
		}
		std::sort(Names.begin(), Names.end());
		if(Fmt == "json") {
			DS::JSONObject Root;
			Root.emplace("bundleVersion", DS::JSON(1.0));
			Root.emplace("kind", DS::JSON(std::string("astraldb.database.v1")));
			DS::JSONObject TablesObj;
			std::shared_lock Guard(DbMutex_);
			for(const std::string &TName : Names) {
				auto SIt = TableSchemas_.find(TName);
				auto TIt = Tables_.find(TName);
				if(SIt == TableSchemas_.end())
					continue;
				const Schema &Sch = SIt->second;
				const HybridTableSlot::Table *RowData = nullptr;
				HybridTableSlot::Table EmptyRows;
				if(TIt != Tables_.end())
					RowData = &TIt->second.RowStore;
				else
					RowData = &EmptyRows;
				DS::JSONArray SchArr;
				for(const Column &Col : Sch) {
					DS::JSONObject Co;
					Co.emplace("name", DS::JSON(Col.Name));
					Co.emplace("sqlType", DS::JSON(Col.DefaultValue));
					Co.emplace("pk", DS::JSON(Col.IsPrimaryKey));
					Co.emplace("unique", DS::JSON(Col.IsUnique));
					Co.emplace("notNull", DS::JSON(Col.IsNotNull));
					SchArr.push_back(DS::JSON(std::move(Co)));
				}
				DS::JSONArray RowsArr;
				for(const Item &Rw : *RowData) {
					DS::JSONObject O;
					for(const Column &Col : Sch) {
						auto It = Rw.find(Col.Name);
						O.emplace(Col.Name,
						           DS::JSON(It != Rw.end() ? It->second : std::string()));
					}
					RowsArr.push_back(DS::JSON(std::move(O)));
				}
				DS::JSONObject TblWrap;
				TblWrap.emplace("schema", DS::JSON(std::move(SchArr)));
				TblWrap.emplace("rows", DS::JSON(std::move(RowsArr)));
				TablesObj.emplace(TName, DS::JSON(std::move(TblWrap)));
			}
			Root.emplace("tables", DS::JSON(std::move(TablesObj)));
			Out << DS::SerializeJSON(DS::JSON(std::move(Root)));
			Out.put('\n');
			if(Logger_)
				Logger_->Info("Exported database bundle (JSON)");
			return true;
		}
		if(Fmt == "csv" || Fmt == "tsv") {
			DS::FormatOptions Opts;
			Opts.Delimiter = Fmt == "tsv" ? '\t' : ',';
			Opts.HasHeader = true;
			DS::TableWriter Writer(Opts);
			std::shared_lock Guard(DbMutex_);
			bool FirstSection = true;
			for(const std::string &TName : Names) {
				auto SIt = TableSchemas_.find(TName);
				auto TIt = Tables_.find(TName);
				if(SIt == TableSchemas_.end())
					continue;
				if(!FirstSection)
					Out.put('\n');
				FirstSection = false;
				const Schema &Sch = SIt->second;
				const Table &Tb =
				    TIt != Tables_.end() ? TIt->second : Table{};
				const DS::Table Exported = BuildTabularForExport(Sch, Tb);
				Out << MakeTableMarker(TName) << '\n';
				if(!Writer.Write(Out, Exported))
					return false;
			}
			if(Logger_)
				Logger_->Info("Exported database bundle (" + Fmt + ")");
			return true;
		}
		if(Logger_)
			Logger_->Error("Unsupported export format");
		return false;
	} catch(const std::exception &Ex) {
		if(Logger_)
			Logger_->Error(std::string("Export failed: ") + Ex.what());
		return false;
	}
}

bool Database::ExportToCSV(std::filesystem::path Destination) {
	return ExportBundle(std::move(Destination), "csv");
}

bool Database::ExportToJSON(std::filesystem::path Destination) {
	return ExportBundle(std::move(Destination), "json");
}

bool Database::ExportToTSV(std::filesystem::path Destination) {
	return ExportBundle(std::move(Destination), "tsv");
}

bool Database::ImportFromCSV(const std::string &TableName, std::filesystem::path Source) {
	try {
		DS::FormatOptions Opts;
		Opts.Delimiter = ',';
		Opts.HasHeader = true;
		DS::TableReader Reader(Opts);
		auto Tb = Reader.ReadFromFile(Source.string());
		if(!Tb || Tb->Headers.empty())
			return false;
		const Schema Sch = SchemaFromHeaders(Tb->Headers, "TEXT");
		if(!TableSchemaSnapshot(TableName))
			CreateTable(TableName, Sch).wait();
		for(const DS::Row &Rw : Tb->Rows) {
			Item Row;
			for(size_t K = 0; K < Tb->Headers.size() && K < Rw.Size(); ++K)
				Row[Tb->Headers[K]] = Rw[K];
			Insert(TableName, Row).wait();
		}
		if(Logger_)
			Logger_->Info("Imported CSV into " + TableName);
		return true;
	} catch(const std::exception &Ex) {
		if(Logger_)
			Logger_->Error(std::string("CSV import failed: ") + Ex.what());
		return false;
	}
}

bool Database::ImportFromTSV(const std::string &TableName, std::filesystem::path Source) {
	try {
		DS::FormatOptions Opts;
		Opts.Delimiter = '\t';
		Opts.HasHeader = true;
		DS::TableReader Reader(Opts);
		auto Tb = Reader.ReadFromFile(Source.string());
		if(!Tb || Tb->Headers.empty())
			return false;
		const Schema Sch = SchemaFromHeaders(Tb->Headers, "TEXT");
		if(!TableSchemaSnapshot(TableName))
			CreateTable(TableName, Sch).wait();
		for(const DS::Row &Rw : Tb->Rows) {
			Item Row;
			for(size_t K = 0; K < Tb->Headers.size() && K < Rw.Size(); ++K)
				Row[Tb->Headers[K]] = Rw[K];
			Insert(TableName, Row).wait();
		}
		if(Logger_)
			Logger_->Info("Imported TSV into " + TableName);
		return true;
	} catch(const std::exception &Ex) {
		if(Logger_)
			Logger_->Error(std::string("TSV import failed: ") + Ex.what());
		return false;
	}
}

bool Database::ImportFromJSON(const std::string &TableName, std::filesystem::path Source) {
	try {
		const DS::JSON Doc = DS::DecodeJSONStrict(ReadPathText(Source));
		DS::JSONArray RowArray;
		if(Doc.IsArray()) {
			RowArray = Doc.AsArray();
		} else if(Doc.IsObject()) {
			const DS::JSONObject &Root = Doc.AsObject();
			if(Root.count("tables") && Root.at("tables").IsObject()) {
				const DS::JSONObject &TO = Root.at("tables").AsObject();
				if(!TO.count(TableName))
					FailStorage("Import bundle: JSON is missing expected table \"" + TableName + "\".");
				const DS::JSON &Wrap = TO.at(TableName);
				if(Wrap.IsObject() && Wrap.AsObject().count("rows"))
					RowArray = Wrap.AsObject().at("rows").AsArray();
				else if(Wrap.IsArray())
					RowArray = Wrap.AsArray();
				else
					FailStorage("Import bundle: invalid table wrapper encoding in JSON payload.");
			} else {
				const auto It = Root.find(TableName);
				if(It != Root.end() && It->second.IsArray())
					RowArray = It->second.AsArray();
				else
					FailStorage("Import bundle: table \"" + TableName + "\" must be a JSON array at the bundle root.");
			}
		} else
			FailStorage("Import bundle: top-level JSON must be an array or an object.");
		const DS::Table Dump = DS::Table::FromJSON(DS::JSON(RowArray), {});
		if(Dump.Headers.empty())
			FailStorage("Import bundle: could not infer any columns from JSON rows (empty or unrecognized shape).");
		if(!TableSchemaSnapshot(TableName))
			CreateTable(TableName, SchemaFromHeaders(Dump.Headers, "TEXT")).wait();
		for(const DS::Row &Rw : Dump.Rows) {
			Item Row;
			for(size_t K = 0; K < Dump.Headers.size() && K < Rw.Size(); ++K)
				Row[Dump.Headers[K]] = Rw[K];
			Insert(TableName, Row).wait();
		}
		if(Logger_)
			Logger_->Info("Imported JSON rows into " + TableName);
		return true;
	} catch(const std::exception &Ex) {
		if(Logger_)
			Logger_->Error(std::string("JSON import failed: ") + Ex.what());
		return false;
	}
}

static Database::Column ParseColumnSpec(const DS::JSONObject &Jo) {
	Database::Column C;
	if(Jo.count("name"))
		C.Name = Jo.at("name").AsString();
	if(Jo.count("sqlType"))
		C.DefaultValue = Jo.at("sqlType").AsString();
	else
		C.DefaultValue = std::string("TEXT");
	if(Jo.count("pk"))
		C.IsPrimaryKey = Jo.at("pk").IsBool() && Jo.at("pk").AsBool();
	if(Jo.count("unique"))
		C.IsUnique = Jo.at("unique").IsBool() && Jo.at("unique").AsBool();
	if(Jo.count("notNull"))
		C.IsNotNull = Jo.at("notNull").IsBool() && Jo.at("notNull").AsBool();
	return C;
}

bool Database::ImportBundle(std::filesystem::path Source, std::string_view FormatIn) {
	try {
		const std::string Fmt = NormFmt(FormatIn);
		if(Fmt == "json") {
			const DS::JSON Doc = DS::DecodeJSONStrict(ReadPathText(Source));
			if(!Doc.IsObject())
				FailStorage("Import bundle: root value must be a JSON object.");
			const DS::JSONObject &Root = Doc.AsObject();
			if(!Root.count("tables") || !Root.at("tables").IsObject())
				FailStorage("Import bundle: missing required \"tables\" object in JSON root.");
			const DS::JSONObject &Tb = Root.at("tables").AsObject();
			std::vector<std::string> Old;
			{
				std::scoped_lock G(DbMutex_);
				Old.reserve(TableSchemas_.size());
				for(const auto &P : TableSchemas_)
					Old.push_back(P.first);
			}
			for(const std::string &N : Old)
				DropTable(N).wait();
			for(const auto &[TName, TVal] : Tb) {
				if(!TVal.IsObject())
					continue;
				const DS::JSONObject &Wrap = TVal.AsObject();
				if(!Wrap.count("schema") || !Wrap.at("schema").IsArray())
					FailStorage("Import bundle: table \"" + TName + "\" has no schema metadata in the bundle.");
				Schema Sch;
				for(const DS::JSON &Jj : Wrap.at("schema").AsArray()) {
					if(!Jj.IsObject())
						continue;
					Sch.push_back(ParseColumnSpec(Jj.AsObject()));
				}
				CreateTable(TName, Sch).wait();
				if(!Wrap.count("rows") || !Wrap.at("rows").IsArray())
					continue;
				for(const DS::JSON &Rj : Wrap.at("rows").AsArray()) {
					if(!Rj.IsObject())
						continue;
					Item Row;
					for(const Column &Cc : Sch) {
						const auto It = Rj.AsObject().find(Cc.Name);
						if(It != Rj.AsObject().end()) {
							if(!It->second.IsNull())
								Row[Cc.Name] =
								    It->second.IsString() ? It->second.AsString()
								                          : DS::SerializeJSON(It->second);
						}
					}
					Insert(TName, Row).wait();
				}
			}
			if(Logger_)
				Logger_->Info("Imported database bundle (JSON)");
			return true;
		}
		if(Fmt == "csv" || Fmt == "tsv") {
			std::vector<std::string> Old;
			{
				std::scoped_lock G(DbMutex_);
				Old.reserve(TableSchemas_.size());
				for(const auto &P : TableSchemas_)
					Old.push_back(P.first);
			}
			for(const std::string &N : Old)
				DropTable(N).wait();
			DS::FormatOptions Opts;
			Opts.Delimiter = Fmt == "tsv" ? '\t' : ',';
			Opts.HasHeader = true;
			std::string Content = ReadPathText(Source);
			std::istringstream In(Content);
			std::string CsvLine;
			std::optional<std::string> PendingName;
			std::ostringstream Block;
			auto FlushPending = [&](const std::string &BlockTxt) -> bool {
				if(!PendingName || BlockTxt.empty())
					return true;
				std::istringstream Bs(BlockTxt);
				DS::TableReader Rd(Opts);
				auto Tb = Rd.Read(Bs);
				if(!Tb)
					return false;
				const Schema Sch = SchemaFromHeaders(Tb->Headers, std::string("TEXT"));
				CreateTable(*PendingName, Sch).wait();
				for(const DS::Row &Rw : Tb->Rows) {
					Item Row;
					for(size_t K = 0; K < Tb->Headers.size() && K < Rw.Size(); ++K)
						Row[Tb->Headers[K]] = Rw[K];
					Insert(*PendingName, Row).wait();
				}
				return true;
			};
			while(std::getline(In, CsvLine)) {
				if(!CsvLine.empty() && CsvLine.back() == '\r')
					CsvLine.pop_back();
				if(auto M = ParseTableMarkerLine(CsvLine)) {
					if(PendingName) {
						std::string S = Block.str();
						Block.str("");
						Block.clear();
						if(!FlushPending(S))
							return false;
					}
					PendingName = std::move(*M);
				} else {
					Block << CsvLine << '\n';
				}
			}
			if(PendingName) {
				std::string S = Block.str();
				return FlushPending(S);
			}
			return true;
		}
		return false;
	} catch(const std::exception &Ex) {
		if(Logger_)
			Logger_->Error(std::string("Import bundle failed: ") + Ex.what());
		return false;
	}
}

bool Database::ConvertTabularFiles(std::filesystem::path SourcePath, std::filesystem::path DestPath,
                                    std::string_view SourceFmt, std::string_view DestFmt) {
	try {
		const std::string S = NormFmt(SourceFmt);
		const std::string D = NormFmt(DestFmt);
		auto ReadCsv = [&](std::filesystem::path P) { return DS::ReadCSV(P.string()); };
		auto ReadTsv = [&](std::filesystem::path P) { return DS::ReadTSV(P.string()); };
		auto ReadJsonTable = [&](std::filesystem::path P) -> DS::Table {
			const DS::JSON J = DS::DecodeJSONStrict(ReadPathText(P));
			if(J.IsArray())
				return DS::Table::FromJSON(J, {});
			if(J.IsObject() && !J.AsObject().empty()) {
				const auto &O = J.AsObject();
				if(O.size() == 1 && O.begin()->second.IsArray())
					return DS::Table::FromJSON(O.begin()->second, {});
			}
			FailStorage("Format convert: JSON source must be an array of objects.");
		};
		DS::Table T;
		if(S == "csv")
			T = ReadCsv(SourcePath);
		else if(S == "tsv")
			T = ReadTsv(SourcePath);
		else if(S == "json")
			T = ReadJsonTable(SourcePath);
		else
			return false;
		if(D == "csv")
			return DS::WriteCSV(T, DestPath.string());
		if(D == "tsv")
			return DS::WriteTSV(T, DestPath.string());
		if(D == "json") {
			std::ofstream Out(DestPath, std::ios::binary);
			if(!Out)
				return false;
			Out << DS::SerializeJSON(DS::TableToJSON(T)) << '\n';
			return true;
		}
		return false;
	} catch(...) {
		return false;
	}
}

void Database::SyncFtsIndexesForTableAssumeLocked(const std::string &TableName) {
	for(auto &[Name, Spec] : FtsIndexes_) {
		(void)Name;
		if(Spec.Table != TableName)
			continue;
		Spec.Index.Clear();
		auto Tit = Tables_.find(TableName);
		if(Tit == Tables_.end())
			continue;
		const auto &Rows = Tit->second.RowStore;
		for(size_t I = 0; I < Rows.size(); ++I) {
			auto It = Rows[I].find(Spec.Column);
			if(It != Rows[I].end())
				Spec.Index.IndexRow(I, It->second);
		}
	}
}

void Database::SyncVectorIndexesForTableAssumeLocked(const std::string &TableName) {
	for(auto &[Name, Spec] : VectorIndexes_) {
		(void)Name;
		if(Spec.Table != TableName)
			continue;
		auto Tit = Tables_.find(TableName);
		if(Tit == Tables_.end()) {
			Spec.Index.Clear();
			continue;
		}
		Spec.Index.BuildFromColumn(Tit->second.RowStore, Spec.Column);
	}
}

void Database::OnRowInsertedAssumeLocked(const std::string &TableName, size_t RowIndex, const Item &Row) {
	(void)RowIndex;
	OnGraphEdgeInsertedAssumeLocked(TableName, Row);
	for(auto &[Name, Spec] : FtsIndexes_) {
		(void)Name;
		if(Spec.Table != TableName)
			continue;
		auto It = Row.find(Spec.Column);
		if(It != Row.end())
			Spec.Index.IndexRow(RowIndex, It->second);
	}
	for(auto &[Name, Spec] : VectorIndexes_) {
		(void)Name;
		if(Spec.Table != TableName)
			continue;
		auto It = Row.find(Spec.Column);
		if(It == Row.end())
			continue;
		const auto Vec = AdvancedTypes::ParseVectorCell(It->second);
		if(Vec)
			Spec.Index.UpsertRow(RowIndex, *Vec);
	}
}

void Database::OnRowUpdatedAssumeLocked(const std::string &TableName, size_t RowIndex, const Item &OldRow,
                                         const Item &NewRow) {
	for(auto &[Name, Spec] : FtsIndexes_) {
		(void)Name;
		if(Spec.Table != TableName)
			continue;
		auto Oit = OldRow.find(Spec.Column);
		if(Oit != OldRow.end())
			Spec.Index.RemoveRow(RowIndex, Oit->second);
		auto Nit = NewRow.find(Spec.Column);
		if(Nit != NewRow.end())
			Spec.Index.IndexRow(RowIndex, Nit->second);
	}
	for(auto &[Name, Spec] : VectorIndexes_) {
		(void)Name;
		if(Spec.Table != TableName)
			continue;
		auto Nit = NewRow.find(Spec.Column);
		if(Nit == NewRow.end()) {
			Spec.Index.RemoveRow(RowIndex);
			continue;
		}
		const auto Vec = AdvancedTypes::ParseVectorCell(Nit->second);
		if(Vec)
			Spec.Index.UpsertRow(RowIndex, *Vec);
		else
			Spec.Index.RemoveRow(RowIndex);
	}
}

void Database::CreateFtsIndex(const std::string &IndexName, const std::string &TableName,
                              const std::string &ColumnName) {
	std::scoped_lock Guard(DbMutex_);
	if(FtsIndexes_.find(IndexName) != FtsIndexes_.end())
		FailStorage("FTS index \"" + IndexName + "\" already exists.");
	if(Tables_.find(TableName) == Tables_.end())
		FailStorage("CREATE INDEX: table \"" + TableName + "\" does not exist.");
	FtsIndexSpec Spec;
	Spec.Table = TableName;
	Spec.Column = ColumnName;
	FtsIndexes_.emplace(IndexName, std::move(Spec));
	SyncFtsIndexesForTableAssumeLocked(TableName);
}

void Database::DropFtsIndex(const std::string &IndexName, bool IfExists) {
	std::scoped_lock Guard(DbMutex_);
	const auto It = FtsIndexes_.find(IndexName);
	if(It == FtsIndexes_.end()) {
		if(!IfExists)
			FailStorage("FTS index \"" + IndexName + "\" does not exist.");
		return;
	}
	FtsIndexes_.erase(It);
}

void Database::CreateVectorIndex(const std::string &IndexName, const std::string &TableName,
                                 const std::string &ColumnName, VectorMetric Metric) {
	std::scoped_lock Guard(DbMutex_);
	if(VectorIndexes_.find(IndexName) != VectorIndexes_.end())
		FailStorage("Vector index \"" + IndexName + "\" already exists.");
	if(Tables_.find(TableName) == Tables_.end())
		FailStorage("CREATE INDEX: table \"" + TableName + "\" does not exist.");
	VectorIndexSpec Spec;
	Spec.Table = TableName;
	Spec.Column = ColumnName;
	Spec.Metric = Metric;
	Spec.Index.SetMetric(Metric);
	VectorIndexes_.emplace(IndexName, std::move(Spec));
	SyncVectorIndexesForTableAssumeLocked(TableName);
}

void Database::DropVectorIndex(const std::string &IndexName, bool IfExists) {
	std::scoped_lock Guard(DbMutex_);
	const auto It = VectorIndexes_.find(IndexName);
	if(It == VectorIndexes_.end()) {
		if(!IfExists)
			FailStorage("Vector index \"" + IndexName + "\" does not exist.");
		return;
	}
	VectorIndexes_.erase(It);
}

void Database::DropSecondaryIndex(const std::string &IndexName, bool IfExists) {
	std::scoped_lock Guard(DbMutex_);
	const bool HadFts = FtsIndexes_.find(IndexName) != FtsIndexes_.end();
	const bool HadVec = VectorIndexes_.find(IndexName) != VectorIndexes_.end();
	if(!HadFts && !HadVec) {
		if(!IfExists)
			FailStorage("Index \"" + IndexName + "\" does not exist.");
		return;
	}
	if(HadFts)
		FtsIndexes_.erase(IndexName);
	if(HadVec)
		VectorIndexes_.erase(IndexName);
}

const TextIndex *Database::FtsForColumn(const std::string &TableName, const std::string &ColumnName) const {
	for(const auto &[Name, Spec] : FtsIndexes_) {
		(void)Name;
		if(Spec.Table == TableName && Spec.Column == ColumnName)
			return &Spec.Index;
	}
	return nullptr;
}

std::vector<size_t> Database::VectorTopKAssumeDbMutexHeld(const std::string &IndexName,
                                                          const std::vector<double> &Query, std::size_t K) const {
	const auto It = VectorIndexes_.find(IndexName);
	if(It == VectorIndexes_.end())
		return {};
	return It->second.Index.TopK(Query, K);
}

std::vector<size_t> Database::VectorTopK(const std::string &IndexName, const std::vector<double> &Query,
                                         std::size_t K) const {
	std::scoped_lock Guard(DbMutex_);
	return VectorTopKAssumeDbMutexHeld(IndexName, Query, K);
}

}