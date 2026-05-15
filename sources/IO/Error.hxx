#pragma once

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace AstralDB {
namespace Err {

/** 1-based line and column for a byte offset in UTF-8 input (counts bytes, not Unicode codepoints). */
inline std::pair<std::size_t, std::size_t> LineColumnAt(std::string_view Source, std::size_t ByteOffset) {
	std::size_t Line = 1, Col = 1;
	const std::size_t N = std::min(ByteOffset, Source.size());
	for(std::size_t I = 0; I < N; ++I) {
		if(Source[I] == '\n') {
			++Line;
			Col = 1;
		} else
			++Col;
	}
	return {Line, Col};
}

inline constexpr std::size_t kDefaultSnippetMargin = 48;

/** Single-line excerpt with a caret under `ByteOffset` (clamped to Source). */
inline std::string SnippetWithCaret(std::string_view Source, std::size_t ByteOffset,
                                    std::size_t Margin = kDefaultSnippetMargin) {
	if(Source.empty())
		return {};
	std::size_t Pos = ByteOffset;
	if(Pos >= Source.size())
		Pos = Source.size() > 0 ? Source.size() - 1 : 0;
	std::size_t LineStart = Pos;
	while(LineStart > 0 && Source[LineStart - 1] != '\n')
		--LineStart;
	std::size_t LineEnd = Pos;
	while(LineEnd < Source.size() && Source[LineEnd] != '\n')
		++LineEnd;
	std::string_view Line = Source.substr(LineStart, LineEnd - LineStart);
	std::size_t Rel = Pos - LineStart;
	std::size_t TrimL = 0, TrimR = Line.size();
	if(Line.size() > 2 * Margin) {
		if(Rel > Margin) {
			TrimL = Rel - Margin;
			Rel = Margin;
		}
		if(Line.size() - TrimL > 2 * Margin)
			TrimR = TrimL + 2 * Margin;
	}
	std::string_view Show = Line.substr(TrimL, TrimR - TrimL);
	const bool EllipsisL = TrimL > 0;
	const bool EllipsisR = TrimR < Line.size();
	std::string Excerpt;
	if(EllipsisL)
		Excerpt += "...";
	Excerpt += Show;
	if(EllipsisR)
		Excerpt += "...";
	std::string Caret(Excerpt.size(), ' ');
	const std::size_t CaretPos =
	    (EllipsisL ? 3 : 0) + Rel - (EllipsisL ? (Rel > Margin ? Rel - Margin : 0) : TrimL);
	if(CaretPos < Caret.size())
		Caret[CaretPos] = '^';
	return Excerpt + '\n' + Caret;
}

inline std::string TruncateToken(std::string_view Tok, std::size_t MaxLen = 32) {
	if(Tok.size() <= MaxLen)
		return std::string(Tok);
	return std::string(Tok.substr(0, MaxLen)) + "...";
}

/** User-facing SQL parse error with location and optional nearby token label. */
inline std::string FormatSqlParse(std::string Message, std::string_view Query, std::size_t ByteOffset,
                                  std::optional<std::string_view> NearToken = std::nullopt) {
	auto [Line, Col] = LineColumnAt(Query, ByteOffset);
	std::string Header =
	    std::format("[SQL parse] line {}, column {}: {}", Line, Col, Message);
	if(NearToken && !NearToken->empty())
		Header += std::format(" (at `{}`)", TruncateToken(*NearToken));
	return Header + '\n' + SnippetWithCaret(Query, ByteOffset);
}

inline std::string FormatSqlLex(std::string Message, std::string_view Query, std::size_t ByteOffset) {
	auto [Line, Col] = LineColumnAt(Query, ByteOffset);
	std::string Header = std::format("[SQL lexer] line {}, column {}: {}", Line, Col, Message);
	return Header + '\n' + SnippetWithCaret(Query, ByteOffset);
}

inline std::string Prefixed(std::string_view Domain, std::string Message) {
	return std::format("[{}] {}", Domain, Message);
}

/** Consistent CLI stderr line(s); message may contain embedded newlines (e.g. SQL parse context). */
inline void PrintCliError(std::ostream &Os, std::string_view Message) {
	Os << "error: " << Message << '\n';
}

} // namespace Err
} // namespace AstralDB
