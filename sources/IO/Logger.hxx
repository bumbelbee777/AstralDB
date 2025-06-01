#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <format>
#include <chrono>
#include <IO/Spinlock.hxx>
#if __cpp_lib_source_location
#include <source_location>
#endif

namespace AstralDB {
class Logger {
	std::ofstream OutputStream_;
	std::vector<std::string> Buffer_;
	Spinlock Mutex_;
	bool Verbose_ = false;
	static constexpr size_t MaxBatch = 16;
	static constexpr const char* ColorBlue = "\033[34m";
	static constexpr const char* ColorYellow = "\033[33m";
	static constexpr const char* ColorRed = "\033[31m";
	static constexpr const char* ColorReset = "\033[0m";

	std::string Timestamp() const;
	void FlushBuffer();
public:
	Logger(const std::string &FilePath, bool Verbose = false);
	~Logger();
#if __cpp_lib_source_location
	void Info(const std::string &Msg, const std::source_location loc = std::source_location::current());
	void Warn(const std::string &Msg, const std::source_location loc = std::source_location::current());
	void Error(const std::string &Msg, const std::source_location loc = std::source_location::current());
#else
	void Info(const std::string &Msg, const char* func = "", const char* file = "", int line = 0);
	void Warn(const std::string &Msg, const char* func = "", const char* file = "", int line = 0);
	void Error(const std::string &Msg, const char* func = "", const char* file = "", int line = 0);
#endif
	void Flush();
	void SetVerbose(bool v) { Verbose_ = v; }
	bool IsVerbose() const { return Verbose_; }
};

// --- Logger member definitions ---
inline std::string Logger::Timestamp() const {
	auto now = std::chrono::system_clock::now();
	auto t_c = std::chrono::system_clock::to_time_t(now);
	std::tm tm;
#if defined(_WIN32)
	localtime_s(&tm, &t_c);
#else
	localtime_r(&t_c, &tm);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	return std::string(buf);
}

inline void Logger::FlushBuffer() {
	for(const auto& Line : Buffer_)
		OutputStream_ << Line << std::endl;
	Buffer_.clear();
}

inline Logger::Logger(const std::string &FilePath, bool Verbose) : Verbose_(Verbose) {
	OutputStream_.open(FilePath, std::ios::app);
	if (!OutputStream_) {
		throw std::runtime_error("Failed to open log file");
	}
}

inline Logger::~Logger() {
	SpinlockGuard lock(Mutex_);
	if (!Buffer_.empty()) FlushBuffer();
	if (OutputStream_.is_open()) {
		OutputStream_.close();
	}
}

#if __cpp_lib_source_location
inline void Logger::Info(const std::string &Msg, const std::source_location loc) {
#else
inline void Logger::Info(const std::string &Msg, const char* func, const char* file, int line) {
#endif
	SpinlockGuard lock(Mutex_);
	if (!Verbose_) return;
	std::string prefix = std::format("{}[INFO]{}", ColorBlue, ColorReset);
	std::string ts = Timestamp();
#if __cpp_lib_source_location
	std::string meta = std::format(" [{}:{}:{}]", loc.function_name(), loc.file_name(), loc.line());
#else
	std::string meta = std::format(" [{}:{}:{}]", func, file, line);
#endif
	Buffer_.push_back(std::format("{} {}{} {}", prefix, ts, meta, Msg));
	if(Buffer_.size() >= MaxBatch) FlushBuffer();
}
#if __cpp_lib_source_location
inline void Logger::Warn(const std::string &Msg, const std::source_location loc) {
#else
inline void Logger::Warn(const std::string &Msg, const char* func, const char* file, int line) {
#endif
	SpinlockGuard lock(Mutex_);
	std::string prefix = std::format("{}[WARN]{}", ColorYellow, ColorReset);
	std::string ts = Timestamp();
#if __cpp_lib_source_location
	std::string meta = std::format(" [{}:{}:{}]", loc.function_name(), loc.file_name(), loc.line());
#else
	std::string meta = std::format(" [{}:{}:{}]", func, file, line);
#endif
	Buffer_.push_back(std::format("{} {}{} {}", prefix, ts, meta, Msg));
	if(Buffer_.size() >= MaxBatch) FlushBuffer();
}
#if __cpp_lib_source_location
inline void Logger::Error(const std::string &Msg, const std::source_location loc) {
#else
inline void Logger::Error(const std::string &Msg, const char* func, const char* file, int line) {
#endif
	SpinlockGuard lock(Mutex_);
	std::string prefix = std::format("{}[ERROR]{}", ColorRed, ColorReset);
	std::string ts = Timestamp();
#if __cpp_lib_source_location
	std::string meta = std::format(" [{}:{}:{}]", loc.function_name(), loc.file_name(), loc.line());
#else
	std::string meta = std::format(" [{}:{}:{}]", func, file, line);
#endif
	Buffer_.push_back(std::format("{} {}{} {}", prefix, ts, meta, Msg));
	if(Buffer_.size() >= MaxBatch) FlushBuffer();
}

inline void Logger::Flush() {
	SpinlockGuard lock(Mutex_);
	FlushBuffer();
}

} // namespace AstralDB