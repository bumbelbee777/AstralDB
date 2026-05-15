#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace AstralDB {

/** Append-only security audit trail (separate from debug \c Logger). */
class AuditLog {
	std::optional<std::filesystem::path> Path_;
	mutable std::mutex Mutex_;

	static std::string TimestampNow() {
		const auto Now = std::chrono::system_clock::now();
		const auto T = std::chrono::system_clock::to_time_t(Now);
		std::tm Tm{};
#if defined(_WIN32)
		localtime_s(&Tm, &T);
#else
		localtime_r(&T, &Tm);
#endif
		char Buf[32];
		std::strftime(Buf, sizeof(Buf), "%Y-%m-%d %H:%M:%S", &Tm);
		return std::string(Buf);
	}

	static std::string SanitizeField(std::string S) {
		for(char &Ch : S) {
			if(Ch == '|' || Ch == '\n' || Ch == '\r')
				Ch = ' ';
		}
		return S;
	}

public:
	AuditLog() = default;
	explicit AuditLog(std::filesystem::path Path) : Path_(std::move(Path)) {}

	void SetPath(std::optional<std::filesystem::path> Path) {
		std::lock_guard Lock(Mutex_);
		Path_ = std::move(Path);
	}

	bool Enabled() const {
		std::lock_guard Lock(Mutex_);
		return Path_.has_value();
	}

	void Record(const std::string &Event, const std::string &Actor, const std::string &Detail,
	            const std::string &Outcome) const {
		std::lock_guard Lock(Mutex_);
		if(!Path_.has_value())
			return;
		std::ofstream Out(*Path_, std::ios::app);
		if(!Out)
			return;
		Out << TimestampNow() << '|' << SanitizeField(Event) << '|' << SanitizeField(Actor) << '|'
		    << SanitizeField(Detail) << '|' << SanitizeField(Outcome) << '\n';
	}
};

} // namespace AstralDB
