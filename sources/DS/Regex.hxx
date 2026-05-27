#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace AstralDB {
namespace DS {
namespace Regex {

/** Bit flags for compilation and matching (PostgreSQL-style \c ~ / \c ~* ). */
enum class Flag : uint32_t {
	None = 0,
	CaseInsensitive = 1u << 0,
	Multiline = 1u << 1,
	DotAll = 1u << 2,
};

inline constexpr Flag operator|(Flag A, Flag B) noexcept {
	return static_cast<Flag>(static_cast<uint32_t>(A) | static_cast<uint32_t>(B));
}

inline constexpr bool HasFlag(Flag Set, Flag Bit) noexcept {
	return (static_cast<uint32_t>(Set) & static_cast<uint32_t>(Bit)) != 0;
}

struct CompileError {
	std::size_t Offset = 0;
	std::string Message;
};

/** Compiled pattern; cheap to copy (shared program). */
class Program {
public:
	Program() = default;
	bool Empty() const noexcept { return !Impl_; }
	explicit operator bool() const noexcept { return static_cast<bool>(Impl_); }

private:
	friend class Compiler;
	friend bool FullMatch(const Program &, std::string_view);
	friend bool Search(const Program &, std::string_view);
	friend bool MatchAt(const Program &, std::string_view, std::size_t Start);

	struct Impl;
	std::shared_ptr<const Impl> Impl_;
};

class Compiler {
public:
	/** Parse and compile \p Pattern. On failure returns error with byte offset. */
	static std::optional<Program> Compile(std::string_view Pattern, Flag Flags = Flag::None,
	                                     CompileError *ErrOut = nullptr);
};

bool FullMatch(const Program &Re, std::string_view Text);
/** Substring search (PostgreSQL \c ~ / SQLite \c REGEXP semantics). */
bool Search(const Program &Re, std::string_view Text);
bool MatchAt(const Program &Re, std::string_view Text, std::size_t Start);

/** One-shot helpers (compile per call; prefer caching \c Program for hot paths). */
bool FullMatch(std::string_view Text, std::string_view Pattern, Flag Flags = Flag::None);
bool Search(std::string_view Text, std::string_view Pattern, Flag Flags = Flag::None);

/** SQL integration: compile-once cache keyed by pattern + flags. */
bool SqlMatch(std::string_view Text, std::string_view Pattern, Flag Flags = Flag::None);

} // namespace Regex
} // namespace DS
} // namespace AstralDB
