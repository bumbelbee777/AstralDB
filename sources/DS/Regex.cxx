#include <DS/Regex.hxx>

#include <IO/Limits.hxx>
#include <IO/SIMD.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace DS {
namespace Regex {
namespace {

using ByteSet = std::array<bool, 256>;

struct Node {
	enum class Kind : uint8_t {
		Empty,
		Literal,
		Any,
		Class,
		Concat,
		Alternate,
		Star,
		Plus,
		Quest,
		AnchorBegin,
		AnchorEnd
	} K = Kind::Empty;
	std::string Lit;
	ByteSet Class{};
	bool ClassNegated = false;
	std::vector<std::unique_ptr<Node>> Children;
};

constexpr std::size_t kSimdLiteralMin = 8;

inline unsigned char FoldByte(unsigned char C, Flag Flags) {
	if(HasFlag(Flags, Flag::CaseInsensitive))
		return static_cast<unsigned char>(std::tolower(C));
	return C;
}

bool BytesEqualFolded(const char *A, const char *B, std::size_t Len, Flag Flags) {
	if(!HasFlag(Flags, Flag::CaseInsensitive)) {
#if defined(__AVX2__)
		std::size_t I = 0;
		for(; I + 32 <= Len; I += 32) {
			__m256i Va = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(A + I));
			__m256i Vb = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(B + I));
			__m256i Eq = _mm256_cmpeq_epi8(Va, Vb);
			if(static_cast<uint32_t>(_mm256_movemask_epi8(Eq)) != 0xFFFFFFFFu)
				return false;
		}
		for(; I < Len; ++I)
			if(A[I] != B[I])
				return false;
		return true;
#elif defined(__SSE2__)
		std::size_t I = 0;
		for(; I + 16 <= Len; I += 16) {
			__m128i Va = _mm_loadu_si128(reinterpret_cast<const __m128i *>(A + I));
			__m128i Vb = _mm_loadu_si128(reinterpret_cast<const __m128i *>(B + I));
			__m128i Eq = _mm_cmpeq_epi8(Va, Vb);
			if(_mm_movemask_epi8(Eq) != 0xFFFF)
				return false;
		}
		for(; I < Len; ++I)
			if(A[I] != B[I])
				return false;
		return true;
#else
		return std::memcmp(A, B, Len) == 0;
#endif
	}
	for(std::size_t I = 0; I < Len; ++I) {
		if(FoldByte(static_cast<unsigned char>(A[I]), Flags) != FoldByte(static_cast<unsigned char>(B[I]), Flags))
			return false;
	}
	return true;
}

bool ClassContains(const ByteSet &Set, bool Negated, unsigned char C, Flag Flags) {
	const unsigned char F = FoldByte(C, Flags);
	const bool Hit = Set[F];
	return Negated ? !Hit : Hit;
}

struct Parser {
	std::string_view Pat;
	std::size_t Pos = 0;
	Flag Flags = Flag::None;
	CompileError *Err = nullptr;

	void Fail(std::size_t Off, std::string Msg) {
		if(Err) {
			Err->Offset = Off;
			Err->Message = std::move(Msg);
		}
	}

	bool AtEnd() const { return Pos >= Pat.size(); }

	char Peek() const { return AtEnd() ? '\0' : Pat[Pos]; }

	char Take() {
		if(AtEnd())
			return '\0';
		return Pat[Pos++];
	}

	bool TakeIf(char C) {
		if(Peek() == C) {
			++Pos;
			return true;
		}
		return false;
	}

	std::optional<unsigned char> ParseEscape() {
		const std::size_t Start = Pos;
		if(AtEnd()) {
			Fail(Start, "Trailing backslash in pattern");
			return std::nullopt;
		}
		const char C = Take();
		switch(C) {
		case 'n':
			return '\n';
		case 'r':
			return '\r';
		case 't':
			return '\t';
		case 'f':
			return '\f';
		case '0':
			return '\0';
		case 'd':
			return static_cast<unsigned char>(128); // sentinel: class
		case 'D':
			return static_cast<unsigned char>(129);
		case 'w':
			return static_cast<unsigned char>(130);
		case 'W':
			return static_cast<unsigned char>(131);
		case 's':
			return static_cast<unsigned char>(132);
		case 'S':
			return static_cast<unsigned char>(133);
		default:
			return static_cast<unsigned char>(C);
		}
	}

	std::unique_ptr<Node> MakeClassFromCode(unsigned char Code) {
		auto N = std::make_unique<Node>();
		N->K = Node::Kind::Class;
		auto FillRange = [&](unsigned char Lo, unsigned char Hi) {
			for(unsigned I = Lo; I <= Hi; ++I)
				N->Class[I] = true;
		};
		switch(Code) {
		case 128:
			FillRange('0', '9');
			break;
		case 129:
			N->ClassNegated = true;
			FillRange('0', '9');
			break;
		case 130:
			for(unsigned I = 0; I < 256; ++I) {
				const unsigned char C = static_cast<unsigned char>(I);
				if(std::isalnum(C) || C == '_')
					N->Class[C] = true;
			}
			break;
		case 131:
			N->ClassNegated = true;
			for(unsigned I = 0; I < 256; ++I) {
				const unsigned char C = static_cast<unsigned char>(I);
				if(std::isalnum(C) || C == '_')
					N->Class[C] = true;
			}
			break;
		case 132:
			for(unsigned char C : {'\t', '\n', '\v', '\f', '\r', ' '})
				N->Class[C] = true;
			break;
		case 133:
			N->ClassNegated = true;
			for(unsigned char C : {'\t', '\n', '\v', '\f', '\r', ' '})
				N->Class[C] = true;
			break;
		default:
			return nullptr;
		}
		return N;
	}

	std::optional<std::unique_ptr<Node>> ParseClass() {
		const std::size_t Start = Pos;
		if(!TakeIf('['))
			return std::nullopt;
		auto N = std::make_unique<Node>();
		N->K = Node::Kind::Class;
		if(TakeIf('^'))
			N->ClassNegated = true;
		if(TakeIf(']'))
			N->Class[static_cast<unsigned char>(']')] = true;
		while(!AtEnd() && Peek() != ']') {
			unsigned char Lo = 0;
			if(Peek() == '\\') {
				++Pos;
				const auto Esc = ParseEscape();
				if(!Esc)
					return std::nullopt;
				if(*Esc >= 128) {
					auto Sub = MakeClassFromCode(*Esc);
					if(!Sub)
						return std::nullopt;
					for(unsigned I = 0; I < 256; ++I) {
						if(Sub->Class[I])
							N->Class[I] = true;
					}
					continue;
				}
				Lo = *Esc;
			} else {
				Lo = static_cast<unsigned char>(Take());
			}
			if(Peek() == '-' && Pos + 1 < Pat.size() && Pat[Pos + 1] != ']') {
				Take();
				unsigned char Hi = 0;
				if(Peek() == '\\') {
					++Pos;
					const auto Esc = ParseEscape();
					if(!Esc)
						return std::nullopt;
					if(*Esc >= 128) {
						Fail(Pos, "Character-class range cannot end with a shorthand escape");
						return std::nullopt;
					}
					Hi = *Esc;
				} else {
					Hi = static_cast<unsigned char>(Take());
				}
				if(Hi < Lo) {
					Fail(Pos, "Invalid character-class range");
					return std::nullopt;
				}
				for(unsigned I = Lo; I <= Hi; ++I)
					N->Class[I] = true;
			} else {
				N->Class[Lo] = true;
			}
		}
		if(!TakeIf(']')) {
			Fail(Start, "Unterminated character class");
			return std::nullopt;
		}
		return N;
	}

	std::optional<std::unique_ptr<Node>> ParseAtom() {
		const std::size_t Start = Pos;
		if(AtEnd()) {
			Fail(Start, "Unexpected end of pattern");
			return std::nullopt;
		}
		if(TakeIf('(')) {
			auto Inner = ParseAlternation();
			if(!Inner)
				return std::nullopt;
			if(!TakeIf(')')) {
				Fail(Pos, "Expected ')'");
				return std::nullopt;
			}
			return Inner;
		}
		if(TakeIf('[')) {
			--Pos;
			return ParseClass();
		}
		if(TakeIf('.')) {
			auto N = std::make_unique<Node>();
			N->K = Node::Kind::Any;
			return N;
		}
		if(TakeIf('^')) {
			auto N = std::make_unique<Node>();
			N->K = Node::Kind::AnchorBegin;
			return N;
		}
		if(TakeIf('$')) {
			auto N = std::make_unique<Node>();
			N->K = Node::Kind::AnchorEnd;
			return N;
		}
		if(TakeIf('|') || TakeIf(')')) {
			Fail(Start, "Unexpected operator in pattern");
			return std::nullopt;
		}
		std::string Lit;
		Lit.push_back(Take());
		while(!AtEnd()) {
			const char C = Peek();
			if(C == '|' || C == ')' || C == '(' || C == '[' || C == ']' || C == '*' || C == '+' || C == '?'
			   || C == '.' || C == '^' || C == '$')
				break;
			if(C == '\\') {
				const std::size_t Save = Pos;
				++Pos;
				const auto Esc = ParseEscape();
				if(!Esc)
					return std::nullopt;
				if(*Esc >= 128) {
					Pos = Save;
					break;
				}
				Lit.push_back(static_cast<char>(*Esc));
				continue;
			}
			Lit.push_back(Take());
		}
		auto N = std::make_unique<Node>();
		N->K = Node::Kind::Literal;
		N->Lit = std::move(Lit);
		return N;
	}

	std::optional<std::unique_ptr<Node>> ParseQuantified() {
		auto Base = ParseAtom();
		if(!Base)
			return std::nullopt;
		while(!AtEnd()) {
			if(TakeIf('*')) {
				auto Wrap = std::make_unique<Node>();
				Wrap->K = Node::Kind::Star;
				Wrap->Children.push_back(std::move(*Base));
				Base = std::move(Wrap);
				continue;
			}
			if(TakeIf('+')) {
				auto Wrap = std::make_unique<Node>();
				Wrap->K = Node::Kind::Plus;
				Wrap->Children.push_back(std::move(*Base));
				Base = std::move(Wrap);
				continue;
			}
			if(TakeIf('?')) {
				auto Wrap = std::make_unique<Node>();
				Wrap->K = Node::Kind::Quest;
				Wrap->Children.push_back(std::move(*Base));
				Base = std::move(Wrap);
				continue;
			}
			break;
		}
		return Base;
	}

	std::optional<std::unique_ptr<Node>> ParseConcat() {
		auto First = ParseQuantified();
		if(!First)
			return std::nullopt;
		std::vector<std::unique_ptr<Node>> Parts;
		Parts.push_back(std::move(*First));
		while(!AtEnd() && Peek() != '|' && Peek() != ')') {
			auto Next = ParseQuantified();
			if(!Next)
				return std::nullopt;
			Parts.push_back(std::move(*Next));
		}
		if(Parts.size() == 1)
			return std::move(Parts[0]);
		auto N = std::make_unique<Node>();
		N->K = Node::Kind::Concat;
		N->Children = std::move(Parts);
		return N;
	}

	std::optional<std::unique_ptr<Node>> ParseAlternation() {
		auto Left = ParseConcat();
		if(!Left)
			return std::nullopt;
		if(!TakeIf('|'))
			return Left;
		std::vector<std::unique_ptr<Node>> Parts;
		Parts.push_back(std::move(*Left));
		while(true) {
			auto Next = ParseConcat();
			if(!Next)
				return std::nullopt;
			Parts.push_back(std::move(*Next));
			if(!TakeIf('|'))
				break;
		}
		auto N = std::make_unique<Node>();
		N->K = Node::Kind::Alternate;
		N->Children = std::move(Parts);
		return N;
	}

	std::optional<std::unique_ptr<Node>> Run() {
		if(Pat.size() > Limits::MaxRegexPatternBytes) {
			Fail(0, "Regex pattern exceeds MaxRegexPatternBytes");
			return std::nullopt;
		}
		auto Root = ParseAlternation();
		if(!Root)
			return std::nullopt;
		if(!AtEnd()) {
			Fail(Pos, "Unexpected trailing syntax in pattern");
			return std::nullopt;
		}
		return Root;
	}
};

void ScanAnchors(const Node *N, bool &Begin, bool &End) {
	if(!N)
		return;
	switch(N->K) {
	case Node::Kind::AnchorBegin:
		Begin = true;
		return;
	case Node::Kind::AnchorEnd:
		End = true;
		return;
	case Node::Kind::Concat:
		for(const auto &C : N->Children)
			ScanAnchors(C.get(), Begin, End);
		return;
	default:
		break;
	}
}

bool MatchNode(const Node *N, std::string_view Text, std::size_t &Pos, Flag Flags, std::size_t Depth);

bool MatchSequencePtrs(const Node *const *Nodes, std::size_t Count, std::size_t Idx, std::string_view Text,
                       std::size_t &Pos, Flag Flags, std::size_t Depth) {
	if(Idx >= Count)
		return true;
	const Node *N = Nodes[Idx];
	if(!N)
		return MatchSequencePtrs(Nodes, Count, Idx + 1, Text, Pos, Flags, Depth);

	const auto MatchRest = [&]() { return MatchSequencePtrs(Nodes, Count, Idx + 1, Text, Pos, Flags, Depth); };

	if((N->K == Node::Kind::Star || N->K == Node::Kind::Plus || N->K == Node::Kind::Quest)
	   && !N->Children.empty()) {
		const Node *Sub = N->Children[0].get();
		if(N->K == Node::Kind::Quest) {
			if(MatchRest())
				return true;
			const std::size_t Save = Pos;
			if(MatchNode(Sub, Text, Pos, Flags, Depth + 1) && MatchRest())
				return true;
			Pos = Save;
			return false;
		}
		bool NeedOne = N->K == Node::Kind::Plus;
		const std::size_t Start = Pos;
		if(!NeedOne && MatchRest())
			return true;
		std::size_t Guard = 0;
		while(Guard++ <= Text.size()) {
			if(!NeedOne || Pos > Start) {
				if(MatchRest())
					return true;
			}
			if(Pos >= Text.size())
				break;
			const std::size_t Before = Pos;
			if(!MatchNode(Sub, Text, Pos, Flags, Depth + 1))
				break;
			if(NeedOne && Pos == Before)
				break;
			NeedOne = false;
		}
		return false;
	}

	if(!MatchNode(N, Text, Pos, Flags, Depth))
		return false;
	return MatchSequencePtrs(Nodes, Count, Idx + 1, Text, Pos, Flags, Depth);
}

bool MatchSequence(const std::vector<std::unique_ptr<Node>> &Nodes, std::size_t Idx, std::string_view Text,
                   std::size_t &Pos, Flag Flags, std::size_t Depth) {
	std::vector<const Node *> Ptrs;
	Ptrs.reserve(Nodes.size());
	for(const auto &C : Nodes)
		Ptrs.push_back(C.get());
	return MatchSequencePtrs(Ptrs.data(), Ptrs.size(), Idx, Text, Pos, Flags, Depth);
}

bool MatchNode(const Node *N, std::string_view Text, std::size_t &Pos, Flag Flags, std::size_t Depth) {
	if(!N)
		return true;
	if(Depth > Limits::MaxRegexMatchDepth)
		return false;
	switch(N->K) {
	case Node::Kind::Empty:
		return true;
	case Node::Kind::Literal: {
		const std::size_t Len = N->Lit.size();
		if(Pos + Len > Text.size())
			return false;
		if(Len >= kSimdLiteralMin) {
			if(!BytesEqualFolded(Text.data() + Pos, N->Lit.data(), Len, Flags))
				return false;
		} else {
			for(std::size_t I = 0; I < Len; ++I) {
				const unsigned char A = static_cast<unsigned char>(Text[Pos + I]);
				const unsigned char B = static_cast<unsigned char>(N->Lit[I]);
				if(FoldByte(A, Flags) != FoldByte(B, Flags))
					return false;
			}
		}
		Pos += Len;
		return true;
	}
	case Node::Kind::Any:
		if(Pos >= Text.size())
			return false;
		if(!HasFlag(Flags, Flag::DotAll) && Text[Pos] == '\n')
			return false;
		++Pos;
		return true;
	case Node::Kind::Class:
		if(Pos >= Text.size())
			return false;
		if(!ClassContains(N->Class, N->ClassNegated, static_cast<unsigned char>(Text[Pos]), Flags))
			return false;
		++Pos;
		return true;
	case Node::Kind::Concat:
		return MatchSequence(N->Children, 0, Text, Pos, Flags, Depth);
	case Node::Kind::Alternate:
		for(const auto &C : N->Children) {
			const std::size_t Save = Pos;
			if(MatchNode(C.get(), Text, Pos, Flags, Depth + 1))
				return true;
			Pos = Save;
		}
		return false;
	case Node::Kind::Star:
	case Node::Kind::Plus:
	case Node::Kind::Quest:
		return MatchSequencePtrs(&N, 1, 0, Text, Pos, Flags, Depth);
	case Node::Kind::AnchorBegin:
		if(Pos != 0) {
			if(!HasFlag(Flags, Flag::Multiline))
				return false;
			if(Pos == 0 || Text[Pos - 1] != '\n')
				return false;
		}
		return true;
	case Node::Kind::AnchorEnd:
		if(Pos != Text.size()) {
			if(!HasFlag(Flags, Flag::Multiline))
				return false;
			if(Pos >= Text.size() || Text[Pos] != '\n')
				return false;
		}
		return true;
	}
	return false;
}

bool RunMatch(Node *Root, Flag Flags, bool AnchorEnd, std::string_view Text, std::size_t Start,
              bool RequireEnd) {
	if(!Root)
		return Text.empty() && Start == 0;
	std::size_t Pos = Start;
	if(!MatchNode(Root, Text, Pos, Flags, 0))
		return false;
	if(RequireEnd || AnchorEnd)
		return Pos == Text.size();
	return true;
}

struct CacheEntry {
	Program Prog;
};

std::mutex &SqlCacheMutex() {
	static std::mutex M;
	return M;
}

std::unordered_map<std::string, CacheEntry> &SqlCache() {
	static std::unordered_map<std::string, CacheEntry> C;
	return C;
}

std::string CacheKey(std::string_view Pattern, Flag Flags) {
	std::string K;
	K.reserve(Pattern.size() + 16);
	K.append(Pattern);
	K.push_back('\x1E');
	K.append(std::to_string(static_cast<uint32_t>(Flags)));
	return K;
}

} // namespace

struct Program::Impl {
	Flag Flags = Flag::None;
	std::unique_ptr<Node> Root;
	bool AnchorBegin = false;
	bool AnchorEnd = false;
};

std::optional<Program> Compiler::Compile(std::string_view Pattern, Flag Flags, CompileError *ErrOut) {
	Parser P;
	P.Pat = Pattern;
	P.Flags = Flags;
	P.Err = ErrOut;
	auto Root = P.Run();
	if(!Root)
		return std::nullopt;
	auto Impl = std::make_shared<Program::Impl>();
	Impl->Flags = Flags;
	Impl->Root = std::move(*Root);
	ScanAnchors(Impl->Root.get(), Impl->AnchorBegin, Impl->AnchorEnd);
	Program Out;
	Out.Impl_ = std::move(Impl);
	return Out;
}

bool FullMatch(const Program &Re, std::string_view Text) {
	if(!Re.Impl_ || !Re.Impl_->Root)
		return Text.empty();
	return RunMatch(Re.Impl_->Root.get(), Re.Impl_->Flags, Re.Impl_->AnchorEnd, Text, 0, true);
}

bool Search(const Program &Re, std::string_view Text) {
	if(!Re.Impl_ || !Re.Impl_->Root)
		return Text.empty();
	const auto &Impl = *Re.Impl_;
	if(Impl.AnchorBegin)
		return RunMatch(Impl.Root.get(), Impl.Flags, Impl.AnchorEnd, Text, 0, Impl.AnchorEnd);
	for(std::size_t I = 0; I <= Text.size(); ++I) {
		if(RunMatch(Impl.Root.get(), Impl.Flags, Impl.AnchorEnd, Text, I, Impl.AnchorEnd))
			return true;
	}
	return false;
}

bool MatchAt(const Program &Re, std::string_view Text, std::size_t Start) {
	if(!Re.Impl_)
		return false;
	return RunMatch(Re.Impl_->Root.get(), Re.Impl_->Flags, Re.Impl_->AnchorEnd, Text, Start, false);
}

bool FullMatch(std::string_view Text, std::string_view Pattern, Flag Flags) {
	const auto Re = Compiler::Compile(Pattern, Flags);
	return Re && FullMatch(*Re, Text);
}

bool Search(std::string_view Text, std::string_view Pattern, Flag Flags) {
	const auto Re = Compiler::Compile(Pattern, Flags);
	return Re && Search(*Re, Text);
}

bool SqlMatch(std::string_view Text, std::string_view Pattern, Flag Flags) {
	if(Pattern.size() > Limits::MaxRegexPatternBytes)
		return false;
	const std::string Key = CacheKey(Pattern, Flags);
	{
		std::lock_guard<std::mutex> Lock(SqlCacheMutex());
		auto It = SqlCache().find(Key);
		if(It != SqlCache().end())
			return Search(It->second.Prog, Text);
	}
	CompileError Err;
	auto Compiled = Compiler::Compile(Pattern, Flags, &Err);
	if(!Compiled)
		return false;
	{
		std::lock_guard<std::mutex> Lock(SqlCacheMutex());
		if(SqlCache().size() >= Limits::MaxRegexCacheEntries)
			SqlCache().clear();
		SqlCache()[Key] = CacheEntry{*Compiled};
	}
	return Search(*Compiled, Text);
}

} // namespace Regex
} // namespace DS
} // namespace AstralDB
