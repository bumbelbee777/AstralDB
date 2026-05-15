#pragma once

#include <IO/Spinlock.hxx>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

class Database;

/** Append-only redo log for durability between checkpoints. Truncated after a successful SyncToFile. */
class WriteAheadLog {
	std::filesystem::path WalPath_;
	Mutex Mut_;
	std::vector<std::string> BufferedLines_;

	void FlushBufferedUnlocked();

public:
	explicit WriteAheadLog(std::filesystem::path DbPath);

	const std::filesystem::path &Path() const { return WalPath_; }

	bool Exists() const { return std::filesystem::exists(WalPath_); }

	void AppendLine(std::string_view Line);
	void Flush();
	void Replay(Database &Db);
	void Truncate();

	~WriteAheadLog();
};

}
