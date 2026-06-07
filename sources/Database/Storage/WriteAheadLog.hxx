#pragma once

#include <Database/Security/AtRestKey.hxx>
#include <IO/Spinlock.hxx>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace AstralDB {

class Database;

struct WalSegmentMetadata {
	std::string TimelineId = "main";
	std::string SegmentStartMarker;
	std::string SegmentEndMarker;
	uint64_t ApproxChecksum = 0;
};

/** Append-only redo log for durability between checkpoints. Truncated after a successful SyncToFile. */
class WriteAheadLog {
	std::filesystem::path WalPath_;
	std::array<uint8_t, 32> WalKey_;
	Mutex Mut_;
	std::vector<std::string> BufferedLines_;
	WalSegmentMetadata SegmentMeta_;

	std::thread AsyncFsyncThread_;
	std::mutex AsyncFsyncMutex_;
	std::condition_variable AsyncFsyncCv_;
	std::queue<std::filesystem::path> AsyncFsyncQueue_;
	std::atomic<bool> StopAsyncFsync_{false};
	std::atomic<std::size_t> PendingAsyncFsync_{0};
	bool WalFsyncCoalesced_ = false;
	std::unordered_set<std::string> PendingPathFsyncKeys_;

#if defined(_WIN32)
	void *WalIoHandle_ = reinterpret_cast<void *>(static_cast<intptr_t>(-1));
#else
	int WalIoFd_ = -1;
#endif

	void FlushBufferedUnlocked();
	void CloseWalIoHandleUnlocked();
	void EnsureWalIoHandleUnlocked();
	void PlatformFsyncWalIoUnlocked();
	void StartAsyncFsyncWorkerIfNeeded();
	void EnqueueAsyncFsyncUnlocked();
	void EnqueuePathFsyncUnlocked(std::filesystem::path Path);
	std::string EncryptRecordToLine(std::string_view PlainLine) const;

public:
	explicit WriteAheadLog(std::filesystem::path DbPath,
	                       const std::array<uint8_t, 32> &EncryptionKey = kAtRestXChaChaKey);

	const std::filesystem::path &Path() const { return WalPath_; }

	bool Exists() const { return std::filesystem::exists(WalPath_); }

	void AppendLine(std::string_view Line);
	void Flush();
	/** When \c ASTRALDB_WAL_FSYNC_ASYNC=1, flush then queue platform fsync on a worker thread. */
	void FlushWithOptionalAsyncFsync();
	void Replay(Database &Db);
	void Truncate();
	/** Block until async fsync queue is drained (no-op when async disabled). */
	void QuiesceAsyncFsync();
	[[nodiscard]] std::size_t PendingAsyncFsyncCount() const noexcept {
		return PendingAsyncFsync_.load(std::memory_order_acquire);
	}
	[[nodiscard]] static bool AsyncFsyncEnabled() noexcept;
	/** Persistent handle append + handle fsync (no read-back); default on with async fsync. */
	[[nodiscard]] static bool ZeroCopyIoEnabled() noexcept;
	/** Queue overlapped platform fsync for an auxiliary path (e.g. main DB snapshot). */
	void SchedulePathFsync(std::filesystem::path Path);
	const WalSegmentMetadata &SegmentMetadata() const { return SegmentMeta_; }
	void SetTimelineId(std::string TimelineId) { SegmentMeta_.TimelineId = std::move(TimelineId); }

	~WriteAheadLog();
};

}
