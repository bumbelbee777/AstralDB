#include <SQL/Procedures/ProcedureParser.hxx>
#include <SQL/Parser/DialectCompat.hxx>
#include <IO/Error.hxx>
#include <IO/Limits.hxx>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AstralDB::SQL {

namespace {

enum class ProcTokKind { End, Ident, Keyword, Number, String, DollarString, Punct };

struct ProcToken {
	ProcTokKind Kind = ProcTokKind::End;
	std::string Text;
	std::size_t Begin = 0;
};

class ProcLexer {
public:
	explicit ProcLexer(std::string_view Source) : Source_(Source) {}

	ProcToken Next() {
		SkipWsAndComments();
		if(Pos_ >= Source_.size())
			return {ProcTokKind::End, {}, Pos_};
		const std::size_t Begin = Pos_;
		const char C = Source_[Pos_];
		if(C == '\'' || C == '"')
			return ReadQuoted(C);
		if(C == '$')
			return ReadDollarQuoted();
		if(std::isdigit(static_cast<unsigned char>(C)))
			return ReadNumber();
		if(std::isalpha(static_cast<unsigned char>(C)) || C == '_')
			return ReadIdent();
		++Pos_;
		return {ProcTokKind::Punct, std::string(1, C), Begin};
	}

	std::size_t Position() const { return Pos_; }

private:
	std::string_view Source_;
	std::size_t Pos_ = 0;

	void SkipWsAndComments() {
		while(Pos_ < Source_.size()) {
			if(std::isspace(static_cast<unsigned char>(Source_[Pos_]))) {
				++Pos_;
				continue;
			}
			if(Pos_ + 1 < Source_.size() && Source_[Pos_] == '-' && Source_[Pos_ + 1] == '-') {
				Pos_ += 2;
				while(Pos_ < Source_.size() && Source_[Pos_] != '\n')
					++Pos_;
				continue;
			}
			if(Pos_ + 1 < Source_.size() && Source_[Pos_] == '/' && Source_[Pos_ + 1] == '*') {
				Pos_ += 2;
				while(Pos_ + 1 < Source_.size()) {
					if(Source_[Pos_] == '*' && Source_[Pos_ + 1] == '/') {
						Pos_ += 2;
						break;
					}
					++Pos_;
				}
				continue;
			}
			break;
		}
	}

	ProcToken ReadQuoted(char Quote) {
		const std::size_t Begin = Pos_;
		++Pos_;
		while(Pos_ < Source_.size()) {
			if(Source_[Pos_] == Quote) {
				++Pos_;
				break;
			}
			if(Source_[Pos_] == '\\' && Pos_ + 1 < Source_.size())
				Pos_ += 2;
			else
				++Pos_;
		}
		return {ProcTokKind::String, std::string(Source_.substr(Begin, Pos_ - Begin)), Begin};
	}

	ProcToken ReadDollarQuoted() {
		const std::size_t Begin = Pos_;
		++Pos_;
		std::size_t TagStart = Pos_;
		while(Pos_ < Source_.size() && Source_[Pos_] != '$')
			++Pos_;
		if(Pos_ >= Source_.size())
			FailLex(Begin, "Unterminated dollar-quote opener.");
		const std::string Tag(Source_.substr(TagStart, Pos_ - TagStart));
		++Pos_;
		const std::string Close = "$" + Tag + "$";
		while(Pos_ + Close.size() <= Source_.size()) {
			if(Source_.compare(Pos_, Close.size(), Close) == 0) {
				Pos_ += Close.size();
				return {ProcTokKind::DollarString,
				        std::string(Source_.substr(Begin, Pos_ - Begin)), Begin};
			}
			++Pos_;
		}
		FailLex(Begin, "Unterminated dollar-quoted procedure body.");
		return {};
	}

	ProcToken ReadNumber() {
		const std::size_t Begin = Pos_;
		while(Pos_ < Source_.size() && (std::isdigit(static_cast<unsigned char>(Source_[Pos_])) || Source_[Pos_] == '.'))
			++Pos_;
		return {ProcTokKind::Number, std::string(Source_.substr(Begin, Pos_ - Begin)), Begin};
	}

	ProcToken ReadIdent() {
		const std::size_t Begin = Pos_;
		while(Pos_ < Source_.size()
		      && (std::isalnum(static_cast<unsigned char>(Source_[Pos_])) || Source_[Pos_] == '_'))
			++Pos_;
		std::string Raw(Source_.substr(Begin, Pos_ - Begin));
		std::string Fold(Raw);
		FoldAsciiUpper(Fold);
		static const char *Kw[] = {"CREATE",     "OR",        "REPLACE",  "ALTER",     "PROCEDURE",
		                           "PROC",       "FUNCTION",  "MACRO",    "IF",        "NOT",
		                           "EXISTS",     "LANGUAGE",  "AS",       "AUTHID",    "DEFINER",
		                           "INVOKER",    "CALL",
		                           "IS",         "BEGIN",     "END",      "DECLARE",   "RETURNS",
		                           "RETURN",     "PERFORM",   "THEN",     "ELSE",      "ELSIF",
		                           "LOOP",       "WHILE",     "FOR",      "CASE",      "RAISE",
		                           "EXCEPTION",  "COMMIT",    "ROLLBACK", "CALL",      "EXECUTE",
		                           "IMMEDIATE",  "TRY",       "CATCH",    "NOCOUNT",   "SET",
		                           "PRINT",      "THROW",     "RAISERROR","STRICT",    "VOLATILE",
		                           "STABLE",     "IMMUTABLE",
		                           "SECURITY",   "DEFINER",   "INVOKER",  "SET",       "SEARCH_PATH",
		                           "COST",       "PARALLEL",  "UNSAFE",   "SAFE",      "RESTRICTED",
		                           "LEAKPROOF",  "NULL",      "ON",       "CONFLICT",  "DO",
		                           "UPDATE",     "INSERT",    "SELECT",   "DELETE",    "INTO",
		                           "FROM",       "WHERE",     "TABLE",    "VIEW",      "INDEX",
		                           "DROP",       "ALTER",     "GRANT",    "REVOKE",    "UNION",
		                           "ALL",        "DISTINCT",  "GROUP",    "ORDER",     "BY",
		                           "HAVING",     "LIMIT",     "OFFSET",   "WITH",      "RECURSIVE",
		                           "VALUES",     "DEFAULT",   "PRIMARY",  "KEY",       "FOREIGN",
		                           "REFERENCES", "CONSTRAINT","CHECK",    "UNIQUE",    "IN"};
		for(const char *K : Kw) {
			if(Fold == K)
				return {ProcTokKind::Keyword, std::string(K), Begin};
		}
		return {ProcTokKind::Ident, std::move(Raw), Begin};
	}

	[[noreturn]] void FailLex(std::size_t Off, std::string Msg) const {
		throw std::runtime_error(Err::FormatSqlLex(std::move(Msg), Source_, Off));
	}
};

bool KeywordIs(const ProcToken &T, const char *Kw) {
	return T.Kind == ProcTokKind::Keyword && T.Text == Kw;
}

bool IdentIs(const ProcToken &T) { return T.Kind == ProcTokKind::Ident; }

void SkipBalancedParens(ProcLexer &Lex) {
	int Depth = 0;
	for(;;) {
		ProcToken T = Lex.Next();
		if(T.Kind == ProcTokKind::End)
			break;
		if(T.Kind == ProcTokKind::Punct && T.Text == "(")
			++Depth;
		else if(T.Kind == ProcTokKind::Punct && T.Text == ")") {
			if(Depth == 0)
				break;
			--Depth;
			if(Depth == 0)
				break;
		}
	}
}

std::string FoldUpper(std::string_view S) {
	std::string U(S);
	FoldAsciiUpper(U);
	return U;
}

struct FoldedDialectSource {
	std::string Folded;
	explicit FoldedDialectSource(std::string_view Source) : Folded(Source) { FoldAsciiUpper(Folded); }
	[[nodiscard]] bool Contains(std::string_view Needle) const {
		std::string N(Needle);
		FoldAsciiUpper(N);
		return Folded.find(N) != std::string::npos;
	}
};

bool SourceContains(std::string_view Hay, std::string_view Needle) {
	return FoldedDialectSource(Hay).Contains(Needle);
}

bool SourceHasTsqlParameter(std::string_view Source) {
	for(std::size_t I = 0; I < Source.size(); ++I) {
		if(Source[I] != '@')
			continue;
		if(I + 1 < Source.size() && (std::isalpha(static_cast<unsigned char>(Source[I + 1])) || Source[I + 1] == '_'))
			return true;
	}
	return false;
}

bool HeaderLooksTsql(std::string_view Source) {
	if(SourceContains(Source, "LANGUAGE") && SourceContains(Source, "PLPGSQL"))
		return false;
	if(Source.find("$$") != std::string::npos)
		return false;
	if(SourceContains(Source, "OR ALTER"))
		return true;
	if(SourceContains(Source, "CREATE PROC ") || SourceContains(Source, "CREATE PROC\n") ||
	   SourceContains(Source, "CREATE PROC\t"))
		return true;
	if(SourceHasTsqlParameter(Source))
		return true;
	if(SourceContains(Source, "BEGIN TRY") || SourceContains(Source, "BEGIN CATCH"))
		return true;
	if(SourceContains(Source, "SET NOCOUNT"))
		return true;
	const std::string U = FoldUpper(Source);
	if(U.find(" AS (") != std::string::npos)
		return false;
	if(SourceContains(Source, " IS") && SourceContains(Source, "BEGIN") && !SourceHasTsqlParameter(Source))
		return false;
	if(SourceContains(Source, " AS") && SourceContains(Source, "BEGIN")) {
		if(SourceContains(Source, "OR REPLACE") && SourceContains(Source, "()") && !SourceHasTsqlParameter(Source) &&
		   !SourceContains(Source, "SET NOCOUNT") && !SourceContains(Source, "BEGIN TRY") &&
		   !SourceContains(Source, "OR ALTER") && !SourceContains(Source, "CREATE PROC "))
			return false;
		return true;
	}
	return false;
}

bool IsWordBoundary(std::string_view Text, std::size_t Pos, std::size_t Len);
bool MatchWord(std::string_view Text, std::size_t Pos, std::string_view Word);
bool IsIfTokenNotEndIf(std::string_view Text, std::size_t IfAt);
std::optional<std::size_t> FindKeywordOutsideQuotes(std::string_view Text, std::string_view Keyword);

void ReplaceElseIfWithElsif(std::string &Body) {
	for(std::size_t I = 0; I + 6 < Body.size();) {
		if(!MatchWord(Body, I, "ELSE")) {
			++I;
			continue;
		}
		std::size_t J = I + 4;
		while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
			++J;
		if(MatchWord(Body, J, "IF")) {
			Body.replace(I, J + 2 - I, "ELSIF");
			I += 5;
			continue;
		}
		++I;
	}
}

void ReplaceKeywordAt(std::string &Body, std::size_t Pos, std::string_view OldWord, std::string_view NewWord) {
	Body.replace(Pos, OldWord.size(), NewWord);
}

std::string NormalizeTsqlTryCatch(std::string_view BodyIn) {
	std::string Body(BodyIn);
	for(std::size_t Pass = 0; Pass < 8; ++Pass) {
		bool Changed = false;
		if(const auto P = FindKeywordOutsideQuotes(Body, "BEGIN TRY")) {
			ReplaceKeywordAt(Body, *P, "BEGIN TRY", "BEGIN");
			Changed = true;
		}
		if(const auto P = FindKeywordOutsideQuotes(Body, "END TRY")) {
			ReplaceKeywordAt(Body, *P, "END TRY", "");
			Changed = true;
		}
		if(const auto P = FindKeywordOutsideQuotes(Body, "BEGIN CATCH")) {
			ReplaceKeywordAt(Body, *P, "BEGIN CATCH", "EXCEPTION WHEN OTHERS THEN");
			Changed = true;
		}
		if(const auto P = FindKeywordOutsideQuotes(Body, "END CATCH")) {
			ReplaceKeywordAt(Body, *P, "END CATCH", "");
			Changed = true;
		}
		if(!Changed)
			break;
	}
	return Body;
}

void TrimInPlace(std::string &S) {
	while(!S.empty() && std::isspace(static_cast<unsigned char>(S.front())))
		S.erase(S.begin());
	while(!S.empty() && std::isspace(static_cast<unsigned char>(S.back())))
		S.pop_back();
}

std::optional<bool> TryEvalConstCondition(std::string_view Raw) {
	std::string C(Raw);
	TrimInPlace(C);
	if(C.empty())
		return std::nullopt;
	FoldAsciiUpper(C);
	if(C == "TRUE" || C == "T" || C == "YES" || C == "1" || C == "1=1" || C == "(TRUE)")
		return true;
	if(C == "FALSE" || C == "F" || C == "NO" || C == "0" || C == "1=0" || C == "1<>1" || C == "1!=1" ||
	   C == "(FALSE)")
		return false;
	if(C.rfind("NOT ", 0) == 0) {
		if(const auto Inner = TryEvalConstCondition(C.substr(4)))
			return !*Inner;
		return std::nullopt;
	}
	const char *Ops[] = {"<>", "!=", "<=", ">=", "=", "<", ">"};
	for(const char *Op : Ops) {
		const std::string Needle(Op);
		const std::size_t At = C.find(Needle);
		if(At == std::string::npos || At == 0)
			continue;
		std::string L = C.substr(0, At);
		std::string R = C.substr(At + Needle.size());
		TrimInPlace(L);
		TrimInPlace(R);
		auto ParseNum = [](const std::string &S) -> std::optional<double> {
			try {
				size_t Pos = 0;
				const double D = std::stod(S, &Pos);
				if(Pos == S.size())
					return D;
			} catch(...) {
			}
			return std::nullopt;
		};
		const auto Ln = ParseNum(L);
		const auto Rn = ParseNum(R);
		if(!Ln || !Rn)
			continue;
		if(Needle == "=")
			return *Ln == *Rn;
		if(Needle == "<>" || Needle == "!=")
			return *Ln != *Rn;
		if(Needle == "<")
			return *Ln < *Rn;
		if(Needle == ">")
			return *Ln > *Rn;
		if(Needle == "<=")
			return *Ln <= *Rn;
		if(Needle == ">=")
			return *Ln >= *Rn;
	}
	return std::nullopt;
}

struct ControlScan {
	bool InString = false;
	char StringQ = 0;
	int IfDepth = 0;
	int LoopDepth = 0;
	int CaseDepth = 0;
	int BeginDepth = 0;
};

bool AdvanceControlScanner(std::string_view Text, std::size_t &I, ControlScan &St) {
	if(I >= Text.size())
		return false;
	const char C = Text[I];
	if(St.InString) {
		if(C == St.StringQ && (I == 0 || Text[I - 1] != '\\'))
			St.InString = false;
		++I;
		return true;
	}
	if(C == '\'' || C == '"') {
		St.InString = true;
		St.StringQ = C;
		++I;
		return true;
	}
	if(C == '$') {
		const std::size_t Open = I;
		++I;
		while(I < Text.size() && Text[I] != '$')
			++I;
		if(I >= Text.size())
			return false;
		const std::string_view Tag = Text.substr(Open + 1, I - Open - 1);
		++I;
		const std::string Close = std::string("$") + std::string(Tag) + "$";
		const std::size_t CloseAt = Text.find(Close, I);
		if(CloseAt == std::string::npos)
			return false;
		I = CloseAt + Close.size();
		return true;
	}
	if(MatchWord(Text, I, "BEGIN")) {
		++St.BeginDepth;
		I += 5;
		return true;
	}
	if(MatchWord(Text, I, "END")) {
		std::size_t J = I + 3;
		while(J < Text.size() && std::isspace(static_cast<unsigned char>(Text[J])))
			++J;
		if(J + 1 < Text.size() && MatchWord(Text, J, "IF")) {
			if(St.IfDepth > 0)
				--St.IfDepth;
			I = J + 2;
			return true;
		}
		if(J + 3 < Text.size() && MatchWord(Text, J, "LOOP")) {
			if(St.LoopDepth > 0)
				--St.LoopDepth;
			I = J + 4;
			return true;
		}
		if(J + 3 < Text.size() && MatchWord(Text, J, "CASE")) {
			if(St.CaseDepth > 0)
				--St.CaseDepth;
			I = J + 4;
			return true;
		}
		if(St.BeginDepth > 0)
			--St.BeginDepth;
		I = J;
		return true;
	}
	if(MatchWord(Text, I, "IF")) {
		++St.IfDepth;
		I += 2;
		return true;
	}
	if(MatchWord(Text, I, "LOOP")) {
		++St.LoopDepth;
		I += 4;
		return true;
	}
	if(MatchWord(Text, I, "WHILE")) {
		++St.LoopDepth;
		I += 5;
		return true;
	}
	if(MatchWord(Text, I, "FOR")) {
		++St.LoopDepth;
		I += 3;
		return true;
	}
	if(MatchWord(Text, I, "CASE")) {
		++St.CaseDepth;
		I += 4;
		return true;
	}
	++I;
	return true;
}

std::size_t SkipUntilThen(std::string_view Text, std::size_t Pos) {
	ControlScan St;
	std::size_t I = Pos;
	int Paren = 0;
	while(I < Text.size()) {
		if(St.InString) {
			AdvanceControlScanner(Text, I, St);
			continue;
		}
		if(Text[I] == '(') {
			++Paren;
			++I;
			continue;
		}
		if(Text[I] == ')') {
			if(Paren > 0)
				--Paren;
			++I;
			continue;
		}
		if(Paren == 0 && MatchWord(Text, I, "THEN")) {
			I += 4;
			return I;
		}
		AdvanceControlScanner(Text, I, St);
	}
	return std::string::npos;
}

std::string ReadUntilIfBranchEnd(std::string_view Text, std::size_t Pos, std::size_t &OutEnd) {
	ControlScan St;
	St.IfDepth = 1;
	std::size_t I = Pos;
	const std::size_t Start = Pos;
	while(I < Text.size()) {
		if(St.IfDepth == 1) {
			if(MatchWord(Text, I, "ELSIF") || MatchWord(Text, I, "ELSE")) {
				OutEnd = I;
				return std::string(Text.substr(Start, I - Start));
			}
			if(MatchWord(Text, I, "END")) {
				std::size_t J = I + 3;
				while(J < Text.size() && std::isspace(static_cast<unsigned char>(Text[J])))
					++J;
				if(MatchWord(Text, J, "IF")) {
					std::size_t K = J + 2;
					while(K < Text.size() && std::isspace(static_cast<unsigned char>(Text[K])))
						++K;
					if(MatchWord(Text, K, "ELSIF") || MatchWord(Text, K, "ELSE")) {
						I = J + 2;
						continue;
					}
					OutEnd = I;
					return std::string(Text.substr(Start, I - Start));
				}
			}
		}
		if(!AdvanceControlScanner(Text, I, St))
			break;
	}
	OutEnd = I;
	return std::string(Text.substr(Start, I - Start));
}

std::string ReadUntilEndLoop(std::string_view Text, std::size_t Pos, std::size_t &OutEnd) {
	ControlScan St;
	St.LoopDepth = 1;
	std::size_t I = Pos;
	const std::size_t Start = Pos;
	while(I < Text.size()) {
		if(St.LoopDepth == 1 && MatchWord(Text, I, "END")) {
			std::size_t J = I + 3;
			while(J < Text.size() && std::isspace(static_cast<unsigned char>(Text[J])))
				++J;
			if(MatchWord(Text, J, "LOOP")) {
				OutEnd = I;
				return std::string(Text.substr(Start, I - Start));
			}
		}
		if(!AdvanceControlScanner(Text, I, St))
			break;
	}
	OutEnd = I;
	return std::string(Text.substr(Start, I - Start));
}

bool TryFoldOneIfBlock(std::string &Body, std::size_t IfAt) {
	std::size_t CondStart = IfAt + 2;
	while(CondStart < Body.size() && std::isspace(static_cast<unsigned char>(Body[CondStart])))
		++CondStart;
	const std::size_t ThenAt = SkipUntilThen(Body, CondStart);
	if(ThenAt == std::string::npos || ThenAt < 4)
		return false;
	const std::size_t ThenPos = ThenAt - 4;
	const std::string Cond = Body.substr(CondStart, ThenPos - CondStart);
	struct Branch {
		std::string Condition;
		std::string Text;
	};
	std::vector<Branch> Branches;
	std::size_t Pos = ThenAt;
	std::size_t BranchEnd = Pos;
	std::string BranchText = ReadUntilIfBranchEnd(Body, Pos, BranchEnd);
	Branches.push_back({Cond, std::move(BranchText)});
	if(BranchEnd >= Body.size())
		return false;
	while(MatchWord(Body, BranchEnd, "ELSIF")) {
		const std::size_t ECondStart = BranchEnd + 5;
		const std::size_t EThenAt = SkipUntilThen(Body, ECondStart);
		if(EThenAt == std::string::npos || EThenAt < 4)
			return false;
		const std::size_t EThenPos = EThenAt - 4;
		const std::string ECond = Body.substr(ECondStart, EThenPos - ECondStart);
		Pos = EThenAt;
		BranchEnd = Pos;
		std::string NextBranch = ReadUntilIfBranchEnd(Body, Pos, BranchEnd);
		Branches.push_back({ECond, std::move(NextBranch)});
		if(BranchEnd >= Body.size())
			return false;
	}
	if(MatchWord(Body, BranchEnd, "ELSE")) {
		Pos = BranchEnd + 4;
		BranchEnd = Pos;
		std::string ElseText = ReadUntilIfBranchEnd(Body, Pos, BranchEnd);
		Branches.push_back({std::string(), std::move(ElseText)});
	}
	if(!MatchWord(Body, BranchEnd, "END"))
		return false;
	std::size_t J = BranchEnd + 3;
	while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
		++J;
	if(!MatchWord(Body, J, "IF"))
		return false;
	std::size_t Tail = J + 2;
	while(Tail < Body.size() && std::isspace(static_cast<unsigned char>(Body[Tail])))
		++Tail;
	if(Tail < Body.size() && Body[Tail] == ';')
		++Tail;
	std::string Chosen;
	for(const auto &B : Branches) {
		if(B.Condition.empty())
			continue;
		const auto Ev = TryEvalConstCondition(B.Condition);
		if(!Ev)
			return false;
		if(*Ev) {
			Chosen = B.Text;
			break;
		}
	}
	if(Chosen.empty()) {
		for(const auto &B : Branches) {
			if(B.Condition.empty())
				Chosen = B.Text;
		}
	}
	Body = Body.substr(0, IfAt) + Chosen + Body.substr(Tail);
	return true;
}

bool TryFoldOneWhileLoop(std::string &Body, std::size_t WhileAt) {
	const std::size_t CondStart = WhileAt + 5;
	const std::size_t LoopKw = Body.find("LOOP", CondStart);
	if(LoopKw == std::string::npos)
		return false;
	std::string Cond = Body.substr(CondStart, LoopKw - CondStart);
	TrimInPlace(Cond);
	const auto Ev = TryEvalConstCondition(Cond);
	if(!Ev)
		return false;
	std::size_t Pos = LoopKw + 4;
	std::size_t BodyEnd = Pos;
	std::string LoopBody = ReadUntilEndLoop(Body, Pos, BodyEnd);
	if(BodyEnd >= Body.size() || !MatchWord(Body, BodyEnd, "END"))
		return false;
	std::size_t J = BodyEnd + 3;
	while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
		++J;
	if(!MatchWord(Body, J, "LOOP"))
		return false;
	std::size_t Tail = J + 4;
	while(Tail < Body.size() && std::isspace(static_cast<unsigned char>(Body[Tail])))
		++Tail;
	if(Tail < Body.size() && Body[Tail] == ';')
		++Tail;
	const std::string Replacement = *Ev ? LoopBody : std::string();
	Body = Body.substr(0, WhileAt) + Replacement + Body.substr(Tail);
	return true;
}

bool TryUnrollOneForLoop(std::string &Body, std::size_t ForAt) {
	const std::size_t InAt = Body.find(" IN ", ForAt);
	const std::size_t LoopKw = Body.find("LOOP", ForAt);
	if(InAt == std::string::npos || LoopKw == std::string::npos || InAt > LoopKw)
		return false;
	std::string Range = Body.substr(InAt + 4, LoopKw - InAt - 4);
	TrimInPlace(Range);
	const std::size_t DotDot = Range.find("..");
	if(DotDot == std::string::npos)
		return false;
	std::string Lo = Range.substr(0, DotDot);
	std::string Hi = Range.substr(DotDot + 2);
	TrimInPlace(Lo);
	TrimInPlace(Hi);
	auto ParseIntLiteral = [](std::string_view S) -> std::optional<long long> {
		std::string T(S);
		TrimInPlace(T);
		try {
			size_t Pos = 0;
			const long long V = std::stoll(T, &Pos);
			if(Pos == T.size())
				return V;
		} catch(...) {
		}
		return std::nullopt;
	};
	const auto LoN = ParseIntLiteral(Lo);
	const auto HiN = ParseIntLiteral(Hi);
	if(!LoN || !HiN)
		return false;
	long long Start = *LoN;
	long long End = *HiN;
	if(Start > End)
		std::swap(Start, End);
	const long long Count = End - Start + 1;
	if(Count <= 0 || Count > 32)
		return false;
	std::size_t Pos = LoopKw + 4;
	std::size_t BodyEnd = Pos;
	std::string LoopBody = ReadUntilEndLoop(Body, Pos, BodyEnd);
	if(BodyEnd >= Body.size() || !MatchWord(Body, BodyEnd, "END"))
		return false;
	std::size_t J = BodyEnd + 3;
	while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
		++J;
	if(!MatchWord(Body, J, "LOOP"))
		return false;
	std::size_t Tail = J + 4;
	while(Tail < Body.size() && std::isspace(static_cast<unsigned char>(Body[Tail])))
		++Tail;
	if(Tail < Body.size() && Body[Tail] == ';')
		++Tail;
	std::string Unrolled;
	for(long long K = 0; K < Count; ++K) {
		if(!Unrolled.empty())
			Unrolled += '\n';
		Unrolled += LoopBody;
	}
	Body = Body.substr(0, ForAt) + Unrolled + Body.substr(Tail);
	return true;
}

struct IfBlockParsed {
	std::vector<std::pair<std::string, std::string>> Branches;
	std::string ElseText;
	std::size_t ConsumedEnd = 0;
};

bool ParseIfBlockAt(std::string_view Body, std::size_t IfAt, IfBlockParsed &Out) {
	if(!MatchWord(Body, IfAt, "IF"))
		return false;
	std::size_t CondStart = IfAt + 2;
	while(CondStart < Body.size() && std::isspace(static_cast<unsigned char>(Body[CondStart])))
		++CondStart;
	const std::size_t ThenAt = SkipUntilThen(Body, CondStart);
	if(ThenAt == std::string::npos || ThenAt < 4)
		return false;
	const std::size_t ThenPos = ThenAt - 4;
	const std::string Cond = std::string(Body.substr(CondStart, ThenPos - CondStart));
	std::size_t Pos = ThenAt;
	std::size_t BranchEnd = Pos;
	std::string BranchText = ReadUntilIfBranchEnd(Body, Pos, BranchEnd);
	Out.Branches.clear();
	Out.Branches.push_back({Cond, std::move(BranchText)});
	if(BranchEnd >= Body.size())
		return false;
	while(MatchWord(Body, BranchEnd, "ELSIF")) {
		const std::size_t ECondStart = BranchEnd + 5;
		const std::size_t EThenAt = SkipUntilThen(Body, ECondStart);
		if(EThenAt == std::string::npos || EThenAt < 4)
			return false;
		const std::size_t EThenPos = EThenAt - 4;
		const std::string ECond = std::string(Body.substr(ECondStart, EThenPos - ECondStart));
		Pos = EThenAt;
		BranchEnd = Pos;
		std::string NextBranch = ReadUntilIfBranchEnd(Body, Pos, BranchEnd);
		Out.Branches.push_back({ECond, std::move(NextBranch)});
		if(BranchEnd >= Body.size())
			return false;
	}
	Out.ElseText.clear();
	if(MatchWord(Body, BranchEnd, "ELSE")) {
		Pos = BranchEnd + 4;
		BranchEnd = Pos;
		Out.ElseText = ReadUntilIfBranchEnd(Body, Pos, BranchEnd);
	}
	if(!MatchWord(Body, BranchEnd, "END"))
		return false;
	std::size_t J = BranchEnd + 3;
	while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
		++J;
	if(!MatchWord(Body, J, "IF"))
		return false;
	Out.ConsumedEnd = J + 2;
	while(Out.ConsumedEnd < Body.size() && std::isspace(static_cast<unsigned char>(Body[Out.ConsumedEnd])))
		++Out.ConsumedEnd;
	if(Out.ConsumedEnd < Body.size() && Body[Out.ConsumedEnd] == ';')
		++Out.ConsumedEnd;
	return true;
}

std::optional<std::size_t> FindNextTopLevelIfOrCase(std::string_view Body, std::size_t Start) {
	ControlScan St;
	std::size_t I = Start;
	while(I < Body.size()) {
		if(St.BeginDepth == 0 && St.IfDepth == 0 && St.LoopDepth == 0 && St.CaseDepth == 0) {
			if(IsIfTokenNotEndIf(Body, I) || MatchWord(Body, I, "CASE"))
				return I;
		}
		if(!AdvanceControlScanner(Body, I, St))
			break;
	}
	return std::nullopt;
}

std::size_t SkipUntilCaseThen(std::string_view Text, std::size_t Pos) {
	ControlScan St;
	St.CaseDepth = 1;
	int Paren = 0;
	std::size_t I = Pos;
	while(I < Text.size()) {
		if(St.InString) {
			AdvanceControlScanner(Text, I, St);
			continue;
		}
		if(Text[I] == '(') {
			++Paren;
			++I;
			continue;
		}
		if(Text[I] == ')') {
			if(Paren > 0)
				--Paren;
			++I;
			continue;
		}
		if(Paren == 0 && St.CaseDepth == 1 && MatchWord(Text, I, "THEN")) {
			I += 4;
			return I;
		}
		AdvanceControlScanner(Text, I, St);
	}
	return std::string::npos;
}

std::string ReadUntilCaseArmEnd(std::string_view Text, std::size_t Pos, std::size_t &OutEnd) {
	ControlScan St;
	St.CaseDepth = 1;
	std::size_t I = Pos;
	const std::size_t Start = Pos;
	while(I < Text.size()) {
		if(St.CaseDepth == 1) {
			if(MatchWord(Text, I, "WHEN") || MatchWord(Text, I, "ELSE")) {
				OutEnd = I;
				return std::string(Text.substr(Start, I - Start));
			}
			if(MatchWord(Text, I, "END")) {
				std::size_t J = I + 3;
				while(J < Text.size() && std::isspace(static_cast<unsigned char>(Text[J])))
					++J;
				if(MatchWord(Text, J, "CASE")) {
					OutEnd = I;
					return std::string(Text.substr(Start, I - Start));
				}
			}
		}
		if(!AdvanceControlScanner(Text, I, St))
			break;
	}
	OutEnd = I;
	return std::string(Text.substr(Start, I - Start));
}

bool ParseCaseBlockAt(std::string_view Body, std::size_t CaseAt, IfBlockParsed &Out) {
	if(!MatchWord(Body, CaseAt, "CASE"))
		return false;
	std::size_t Pos = CaseAt + 4;
	while(Pos < Body.size() && std::isspace(static_cast<unsigned char>(Body[Pos])))
		++Pos;
	std::string CaseExpr;
	if(!MatchWord(Body, Pos, "WHEN")) {
		const std::size_t ExprStart = Pos;
		while(Pos < Body.size() && !MatchWord(Body, Pos, "WHEN"))
			++Pos;
		CaseExpr = std::string(Body.substr(ExprStart, Pos - ExprStart));
		TrimInPlace(CaseExpr);
	}
	Out.Branches.clear();
	Out.ElseText.clear();
	while(Pos < Body.size() && MatchWord(Body, Pos, "WHEN")) {
		Pos += 4;
		const std::size_t CondStart = Pos;
		const std::size_t ThenAt = SkipUntilCaseThen(Body, CondStart);
		if(ThenAt == std::string::npos || ThenAt < 4)
			return false;
		std::string Cond = std::string(Body.substr(CondStart, ThenAt - 4 - CondStart));
		TrimInPlace(Cond);
		if(!CaseExpr.empty()) {
			std::string Wrapped = "(" + CaseExpr + ") = (" + Cond + ")";
			Cond = std::move(Wrapped);
		}
		Pos = ThenAt;
		std::size_t ArmEnd = Pos;
		std::string ArmBody = ReadUntilCaseArmEnd(Body, Pos, ArmEnd);
		Out.Branches.push_back({std::move(Cond), std::move(ArmBody)});
		Pos = ArmEnd;
	}
	if(MatchWord(Body, Pos, "ELSE")) {
		Pos += 4;
		std::size_t ElseEnd = Pos;
		Out.ElseText = ReadUntilCaseArmEnd(Body, Pos, ElseEnd);
		Pos = ElseEnd;
	}
	if(!MatchWord(Body, Pos, "END"))
		return false;
	std::size_t J = Pos + 3;
	while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
		++J;
	if(!MatchWord(Body, J, "CASE"))
		return false;
	Out.ConsumedEnd = J + 4;
	while(Out.ConsumedEnd < Body.size() && std::isspace(static_cast<unsigned char>(Body[Out.ConsumedEnd])))
		++Out.ConsumedEnd;
	if(Out.ConsumedEnd < Body.size() && Body[Out.ConsumedEnd] == ';')
		++Out.ConsumedEnd;
	return !Out.Branches.empty();
}

std::string FoldConstantControlFlow(std::string_view BodyIn) {
	std::string Body(BodyIn);
	for(bool Progress = true; Progress;) {
		Progress = false;
		for(std::size_t I = 0; I + 2 < Body.size();) {
			if(IsIfTokenNotEndIf(Body, I) && TryFoldOneIfBlock(Body, I)) {
				Progress = true;
				I = 0;
				continue;
			}
			if(MatchWord(Body, I, "WHILE") && TryFoldOneWhileLoop(Body, I)) {
				Progress = true;
				I = 0;
				continue;
			}
			if(MatchWord(Body, I, "FOR") && TryUnrollOneForLoop(Body, I)) {
				Progress = true;
				I = 0;
				continue;
			}
			++I;
		}
	}
	return Body;
}

struct BodySlice {
	std::string_view Text;
	ProcedureDialectKind Dialect = ProcedureDialectKind::PlSql;
};

std::optional<BodySlice> ExtractDollarBody(std::string_view Source, std::size_t AfterAs) {
	std::size_t P = AfterAs;
	while(P < Source.size() && std::isspace(static_cast<unsigned char>(Source[P])))
		++P;
	if(P >= Source.size() || Source[P] != '$')
		return std::nullopt;
	const std::size_t Open = P;
	++P;
	while(P < Source.size() && Source[P] != '$')
		++P;
	if(P >= Source.size())
		return std::nullopt;
	const std::string_view Tag = Source.substr(Open + 1, P - Open - 1);
	++P;
	const std::string Close = std::string("$") + std::string(Tag) + "$";
	const std::size_t CloseAt = Source.find(Close, P);
	if(CloseAt == std::string::npos)
		return std::nullopt;
	const std::string_view Inner = Source.substr(P, CloseAt - P);
	return BodySlice{Inner, ProcedureDialectKind::PlPgSql};
}

std::optional<std::size_t> FindKeywordOutsideQuotes(std::string_view Text, std::string_view Keyword) {
	const std::string Kw = FoldUpper(Keyword);
	std::size_t I = 0;
	while(I < Text.size()) {
		if(Text[I] == '\'' || Text[I] == '"') {
			const char Q = Text[I];
			++I;
			while(I < Text.size() && Text[I] != Q) {
				if(Text[I] == '\\' && I + 1 < Text.size())
					I += 2;
				else
					++I;
			}
			if(I < Text.size())
				++I;
			continue;
		}
		if(Text[I] == '$') {
			++I;
			const std::size_t Tag0 = I;
			while(I < Text.size() && Text[I] != '$')
				++I;
			if(I >= Text.size())
				break;
			const std::string_view Tag = Text.substr(Tag0, I - Tag0);
			++I;
			const std::string Close = std::string("$") + std::string(Tag) + "$";
			const std::size_t CloseAt = Text.find(Close, I);
			if(CloseAt == std::string::npos)
				break;
			I = CloseAt + Close.size();
			continue;
		}
		if(std::isalpha(static_cast<unsigned char>(Text[I])) || Text[I] == '_') {
			const std::size_t Start = I;
			++I;
			while(I < Text.size() && (std::isalnum(static_cast<unsigned char>(Text[I])) || Text[I] == '_'))
				++I;
			if(FoldUpper(Text.substr(Start, I - Start)) == Kw)
				return Start;
			continue;
		}
		++I;
	}
	return std::nullopt;
}

std::optional<BodySlice> ExtractBeginEndBody(std::string_view Source, ProcedureDialectKind Dialect) {
	const auto BeginPos = FindKeywordOutsideQuotes(Source, "BEGIN");
	if(!BeginPos)
		return std::nullopt;
	std::size_t Pos = *BeginPos + 5;
	while(Pos < Source.size() && std::isspace(static_cast<unsigned char>(Source[Pos])))
		++Pos;
	const std::size_t InnerStart = Pos;
	int BeginDepth = 1;
	while(Pos < Source.size() && BeginDepth > 0) {
		if(Source[Pos] == '\'' || Source[Pos] == '"') {
			const char Q = Source[Pos];
			++Pos;
			while(Pos < Source.size() && Source[Pos] != Q) {
				if(Source[Pos] == '\\' && Pos + 1 < Source.size())
					Pos += 2;
				else
					++Pos;
			}
			if(Pos < Source.size())
				++Pos;
			continue;
		}
		if(Source[Pos] == '$') {
			++Pos;
			const std::size_t Tag0 = Pos;
			while(Pos < Source.size() && Source[Pos] != '$')
				++Pos;
			if(Pos >= Source.size())
				break;
			const std::string_view Tag = Source.substr(Tag0, Pos - Tag0);
			++Pos;
			const std::string Close = std::string("$") + std::string(Tag) + "$";
			const std::size_t CloseAt = Source.find(Close, Pos);
			if(CloseAt == std::string::npos)
				break;
			Pos = CloseAt + Close.size();
			continue;
		}
		if(std::isalpha(static_cast<unsigned char>(Source[Pos])) || Source[Pos] == '_') {
			const std::size_t W0 = Pos;
			++Pos;
			while(Pos < Source.size() && (std::isalnum(static_cast<unsigned char>(Source[Pos])) || Source[Pos] == '_'))
				++Pos;
			const std::string Word = FoldUpper(Source.substr(W0, Pos - W0));
			if(Word == "BEGIN")
				++BeginDepth;
			else if(Word == "END") {
				std::size_t J = Pos;
				while(J < Source.size() && std::isspace(static_cast<unsigned char>(Source[J])))
					++J;
				if(J + 1 < Source.size() && MatchWord(Source, J, "IF")) {
					Pos = J + 2;
					continue;
				}
				if(J + 3 < Source.size() && MatchWord(Source, J, "LOOP")) {
					Pos = J + 4;
					continue;
				}
				if(J + 3 < Source.size() && MatchWord(Source, J, "CASE")) {
					Pos = J + 4;
					continue;
				}
				--BeginDepth;
				if(BeginDepth == 0) {
					std::string_view Inner = Source.substr(InnerStart, W0 - InnerStart);
					while(!Inner.empty() && std::isspace(static_cast<unsigned char>(Inner.back())))
						Inner.remove_suffix(1);
					return BodySlice{Inner, Dialect};
				}
			}
			continue;
		}
		++Pos;
	}
	return std::nullopt;
}

void StripDeclarePrefix(std::string_view &Body) {
	const auto Decl = FindKeywordOutsideQuotes(Body, "DECLARE");
	if(!Decl || *Decl != 0)
		return;
	const auto Begin = FindKeywordOutsideQuotes(Body, "BEGIN");
	if(!Begin)
		return;
	Body = Body.substr(*Begin);
}

std::optional<std::string> ExtractFirstQuotedLiteral(std::string_view Stmt) {
	for(std::size_t I = 0; I < Stmt.size(); ++I) {
		if(Stmt[I] == 'N' && I + 1 < Stmt.size() && Stmt[I + 1] == '\'')
			++I;
		if(Stmt[I] != '\'' && Stmt[I] != '"')
			continue;
		const char Q = Stmt[I++];
		std::string Out;
		while(I < Stmt.size() && Stmt[I] != Q) {
			if(Stmt[I] == '\\' && I + 1 < Stmt.size()) {
				Out += Stmt[I + 1];
				I += 2;
			} else
				Out += Stmt[I++];
		}
		return Out;
	}
	return std::nullopt;
}

std::string RewriteStatementForAstral(std::string Stmt, ProcedureDialectKind Dialect) {
	std::string Fold(Stmt);
	FoldAsciiUpper(Fold);
	std::size_t Lead = 0;
	while(Lead < Fold.size() && std::isspace(static_cast<unsigned char>(Fold[Lead])))
		++Lead;
	Fold = Fold.substr(Lead);
	Stmt.erase(0, Lead);
	if(Fold.empty())
		return {};
	if(Fold.rfind("PERFORM ", 0) == 0 && Dialect == ProcedureDialectKind::PlPgSql) {
		std::string Out = "SELECT ";
		Out += Stmt.substr(8);
		return Out;
	}
	if(Fold.rfind("RETURN QUERY ", 0) == 0 &&
	   (Dialect == ProcedureDialectKind::PlPgSql || Dialect == ProcedureDialectKind::DuckDb)) {
		std::string Out = "SELECT ";
		Out += Stmt.substr(13);
		return Out;
	}
	if(Fold.rfind("EXECUTE IMMEDIATE ", 0) == 0 &&
	   (Dialect == ProcedureDialectKind::PlSql || Dialect == ProcedureDialectKind::PlPgSql ||
	    Dialect == ProcedureDialectKind::DuckDb)) {
		if(const auto Lit = ExtractFirstQuotedLiteral(Stmt))
			return *Lit;
		throw std::runtime_error(
		    Err::Prefixed("PROC", "EXECUTE IMMEDIATE requires a string literal to lower."));
	}
	if(Fold.rfind("EXECUTE ", 0) == 0 && Fold.find("IMMEDIATE") == std::string::npos &&
	   (Dialect == ProcedureDialectKind::PlPgSql || Dialect == ProcedureDialectKind::PlSql)) {
		if(const auto Lit = ExtractFirstQuotedLiteral(Stmt))
			return *Lit;
		return {};
	}
	if(Fold.rfind("GET DIAGNOSTICS", 0) == 0 || Fold.rfind("ASSERT ", 0) == 0 || Fold.rfind("PRAGMA ", 0) == 0)
		return {};
	if(Fold == "NULL")
		return {};
	if(Dialect == ProcedureDialectKind::TSql) {
		if(Fold.rfind("DECLARE ", 0) == 0 && Fold.find('@') != std::string::npos)
			return {};
		if(Fold.rfind("SET ", 0) == 0)
			return {};
		if(Fold.rfind("PRINT ", 0) == 0 || Fold == "PRINT")
			return {};
		if(Fold == "RETURN" || Fold.rfind("RETURN ", 0) == 0)
			return {};
		if(Fold.rfind("THROW ", 0) == 0 || Fold.rfind("RAISERROR ", 0) == 0)
			return {};
		if(Fold.rfind("WAITFOR ", 0) == 0 || Fold.rfind("GOTO ", 0) == 0)
			return {};
		if(Fold.rfind("BEGIN TRAN", 0) == 0 || Fold.rfind("BEGIN TRANSACTION", 0) == 0)
			return {};
		if(Fold.rfind("COMMIT TRAN", 0) == 0 || Fold.rfind("COMMIT TRANSACTION", 0) == 0)
			return {};
		if(Fold.rfind("ROLLBACK TRAN", 0) == 0 || Fold.rfind("ROLLBACK TRANSACTION", 0) == 0)
			return {};
		if(Fold.rfind("SET @", 0) == 0)
			return {};
		if(Fold.rfind("SELECT @", 0) == 0 && Fold.find('=') != std::string::npos)
			return {};
		if(Fold.find("SP_EXECUTESQL") != std::string::npos) {
			if(const auto Lit = ExtractFirstQuotedLiteral(Stmt))
				return *Lit;
			return {};
		}
		if(Fold.rfind("EXEC ", 0) == 0 || Fold.rfind("EXECUTE ", 0) == 0) {
			if(const auto Lit = ExtractFirstQuotedLiteral(Stmt))
				return *Lit;
			return {};
		}
	}
	if(Fold == "COMMIT" || Fold == "COMMIT WORK" || Fold == "ROLLBACK" || Fold == "ROLLBACK WORK")
		return {};
	if(Fold.rfind("RAISE ", 0) == 0)
		return {};
	return Stmt;
}

struct ScanState {
	int BeginDepth = 0;
	int IfDepth = 0;
	int LoopDepth = 0;
	int CaseDepth = 0;
	bool InString = false;
	char StringQ = 0;
};

bool IsWordBoundary(std::string_view Text, std::size_t Pos, std::size_t Len) {
	const auto IsIdentChar = [](char C) {
		return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
	};
	const bool Left = Pos == 0 || !IsIdentChar(Text[Pos - 1]);
	const bool Right = Pos + Len >= Text.size() || !IsIdentChar(Text[Pos + Len]);
	return Left && Right;
}

bool MatchWord(std::string_view Text, std::size_t Pos, std::string_view Word) {
	if(Pos + Word.size() > Text.size())
		return false;
	for(std::size_t I = 0; I < Word.size(); ++I) {
		const char A = Text[Pos + I];
		const char B = Word[I];
		if(std::toupper(static_cast<unsigned char>(A)) != std::toupper(static_cast<unsigned char>(B)))
			return false;
	}
	return IsWordBoundary(Text, Pos, Word.size());
}

bool IsIfTokenNotEndIf(std::string_view Text, std::size_t IfAt) {
	if(!MatchWord(Text, IfAt, "IF"))
		return false;
	std::size_t J = IfAt;
	while(J > 0 && std::isspace(static_cast<unsigned char>(Text[J - 1])))
		--J;
	return !(J >= 3 && MatchWord(Text, J - 3, "END"));
}

bool StmtStartsTsqlAction(std::string_view Text, std::size_t Pos) {
	static const char *Words[] = {"INSERT", "SELECT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER",
	                              "EXEC",   "EXECUTE", "MERGE",  "TRUNCATE", "WITH",  "CALL"};
	for(const char *W : Words) {
		if(MatchWord(Text, Pos, W))
			return true;
	}
	return false;
}

bool TryNormalizeTsqlIfSingleStmtToThen(std::string &Body, std::size_t IfAt) {
	if(!IsIfTokenNotEndIf(Body, IfAt))
		return false;
	std::size_t I = IfAt + 2;
	ControlScan St;
	int Paren = 0;
	while(I < Body.size()) {
		if(St.InString) {
			AdvanceControlScanner(Body, I, St);
			continue;
		}
		if(Body[I] == '(') {
			++Paren;
			++I;
			continue;
		}
		if(Body[I] == ')') {
			if(Paren > 0)
				--Paren;
			++I;
			continue;
		}
		if(Paren == 0) {
			if(MatchWord(Body, I, "THEN") || MatchWord(Body, I, "BEGIN"))
				return false;
			if(StmtStartsTsqlAction(Body, I)) {
				Body.insert(I, "THEN ");
				std::size_t J = I + 5;
				ControlScan EndSt;
				int EndParen = 0;
				while(J < Body.size()) {
					if(EndSt.InString) {
						AdvanceControlScanner(Body, J, EndSt);
						continue;
					}
					if(Body[J] == '(') {
						++EndParen;
						++J;
						continue;
					}
					if(Body[J] == ')') {
						if(EndParen > 0)
							--EndParen;
						++J;
						continue;
					}
					if(EndParen == 0 && Body[J] == ';') {
						Body.insert(J + 1, " END IF");
						return true;
					}
					if(EndParen == 0 && (MatchWord(Body, J, "ELSE") || MatchWord(Body, J, "ELSIF") ||
					                     MatchWord(Body, J, "END"))) {
						Body.insert(J, " END IF ");
						return true;
					}
					AdvanceControlScanner(Body, J, EndSt);
				}
				Body += " END IF";
				return true;
			}
		}
		AdvanceControlScanner(Body, I, St);
	}
	return false;
}

bool TryNormalizeTsqlIfBeginToThen(std::string &Body, std::size_t IfAt) {
	const bool IsElsif = MatchWord(Body, IfAt, "ELSIF");
	if(!IsIfTokenNotEndIf(Body, IfAt) && !IsElsif)
		return false;
	const std::size_t CondStart = IfAt + (IsElsif ? 5 : 2);
	if(SkipUntilThen(Body, CondStart) != std::string::npos)
		return false;
	int Paren = 0;
	ControlScan St;
	std::size_t I = CondStart;
	while(I < Body.size()) {
		if(St.InString) {
			AdvanceControlScanner(Body, I, St);
			continue;
		}
		if(Body[I] == '(')
			++Paren;
		else if(Body[I] == ')') {
			if(Paren > 0)
				--Paren;
			++I;
			continue;
		}
		if(Paren == 0 && MatchWord(Body, I, "BEGIN")) {
			Body.replace(I, 5, "THEN");
			return true;
		}
		AdvanceControlScanner(Body, I, St);
	}
	return false;
}

bool TryNormalizeTsqlWhileBeginToLoop(std::string &Body, std::size_t WhileAt) {
	const std::size_t CondStart = WhileAt + 5;
	int Paren = 0;
	ControlScan St;
	std::size_t I = CondStart;
	while(I < Body.size()) {
		if(St.InString) {
			AdvanceControlScanner(Body, I, St);
			continue;
		}
		if(Body[I] == '(')
			++Paren;
		else if(Body[I] == ')') {
			if(Paren > 0)
				--Paren;
			++I;
			continue;
		}
		if(Paren == 0 && MatchWord(Body, I, "BEGIN")) {
			Body.replace(I, 5, "LOOP");
			return true;
		}
		if(Paren == 0 && MatchWord(Body, I, "LOOP"))
			return false;
		AdvanceControlScanner(Body, I, St);
	}
	return false;
}

bool TryAppendEndIfForTsqlIf(std::string &Body, std::size_t EndAt) {
	std::size_t J = EndAt + 3;
	while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
		++J;
	if(MatchWord(Body, J, "IF") || MatchWord(Body, J, "LOOP") || MatchWord(Body, J, "CASE"))
		return false;
	if(J >= Body.size() || Body[J] == ';' || MatchWord(Body, J, "ELSE") || MatchWord(Body, J, "ELSIF") ||
	   MatchWord(Body, J, "END")) {
		Body.insert(EndAt + 3, " IF");
		return true;
	}
	return false;
}

bool TryAppendEndLoopForTsqlWhile(std::string &Body, std::size_t EndAt) {
	std::size_t J = EndAt + 3;
	while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
		++J;
	if(MatchWord(Body, J, "LOOP") || MatchWord(Body, J, "IF") || MatchWord(Body, J, "CASE"))
		return false;
	const std::string Prefix(Body.substr(0, EndAt));
	if(Prefix.find("LOOP") == std::string::npos)
		return false;
	if(MatchWord(Body, J, "ELSE"))
		return false;
	Body.insert(EndAt + 3, " LOOP");
	return true;
}

std::string NormalizeTsqlControlFlow(std::string Body) {
	ReplaceElseIfWithElsif(Body);
	Body = NormalizeTsqlTryCatch(Body);
	for(bool Progress = true; Progress;) {
		Progress = false;
		for(std::size_t I = 0; I + 2 < Body.size(); ++I) {
			if(IsIfTokenNotEndIf(Body, I) && TryNormalizeTsqlIfSingleStmtToThen(Body, I)) {
				Progress = true;
				break;
			}
		}
	}
	for(bool Progress = true; Progress;) {
		Progress = false;
		for(std::size_t I = 0; I + 2 < Body.size(); ++I) {
			if((IsIfTokenNotEndIf(Body, I) || MatchWord(Body, I, "ELSIF")) && TryNormalizeTsqlIfBeginToThen(Body, I)) {
				Progress = true;
				break;
			}
		}
	}
	for(bool Progress = true; Progress;) {
		Progress = false;
		for(std::size_t I = 0; I + 5 < Body.size(); ++I) {
			if(MatchWord(Body, I, "WHILE") && TryNormalizeTsqlWhileBeginToLoop(Body, I)) {
				Progress = true;
				break;
			}
		}
	}
	for(bool Progress = true; Progress;) {
		Progress = false;
		for(std::size_t I = 0; I + 2 < Body.size(); ++I) {
			if(MatchWord(Body, I, "END") && TryAppendEndIfForTsqlIf(Body, I)) {
				Progress = true;
				break;
			}
		}
	}
	for(bool Progress = true; Progress;) {
		Progress = false;
		for(std::size_t I = 0; I + 2 < Body.size(); ++I) {
			if(MatchWord(Body, I, "END") && TryAppendEndLoopForTsqlWhile(Body, I)) {
				Progress = true;
				break;
			}
		}
	}
	return Body;
}

std::string LowerDialectBodyLinear(std::string_view BodyText, ProcedureDialectKind Dialect, bool SkipConstFold) {
	std::string_view Body = BodyText;
	if(auto Block = ExtractBeginEndBody(Body, Dialect))
		Body = Block->Text;
	StripDeclarePrefix(Body);
	std::string NormalizedBody = SkipConstFold ? std::string(Body) : FoldConstantControlFlow(Body);
	Body = NormalizedBody;
	if(Body.empty())
		return {};
	if(Body.size() > Limits::MaxSqlSourceBytes)
		throw std::runtime_error(Err::Prefixed("PROC", "Procedure body exceeds size guard."));

	std::string Out;
	std::string Cur;
	ScanState St;
	for(std::size_t I = 0; I < Body.size(); ++I) {
		const char C = Body[I];
		if(St.InString) {
			Cur += C;
			if(C == St.StringQ && (I == 0 || Body[I - 1] != '\\'))
				St.InString = false;
			continue;
		}
		if(C == '\'' || C == '"') {
			St.InString = true;
			St.StringQ = C;
			Cur += C;
			continue;
		}
		if(C == '$') {
			const std::size_t Open = I;
			++I;
			while(I < Body.size() && Body[I] != '$')
				++I;
			if(I >= Body.size())
				throw std::runtime_error(Err::Prefixed("PROC", "Unterminated dollar quote in procedure body."));
			const std::string_view Tag = Body.substr(Open + 1, I - Open - 1);
			++I;
			const std::string Close = std::string("$") + std::string(Tag) + "$";
			const std::size_t CloseAt = Body.find(Close, I);
			if(CloseAt == std::string::npos)
				throw std::runtime_error(Err::Prefixed("PROC", "Unterminated dollar quote in procedure body."));
			Cur.append(Body.substr(Open, CloseAt + Close.size() - Open));
			I = CloseAt + Close.size() - 1;
			continue;
		}
		if(MatchWord(Body, I, "BEGIN")) {
			++St.BeginDepth;
			I += 4;
			continue;
		}
		if(MatchWord(Body, I, "END")) {
			std::size_t J = I + 3;
			while(J < Body.size() && std::isspace(static_cast<unsigned char>(Body[J])))
				++J;
			if(J < Body.size() && std::isalpha(static_cast<unsigned char>(Body[J]))) {
				while(J < Body.size() && (std::isalnum(static_cast<unsigned char>(Body[J])) || Body[J] == '_'))
					++J;
			} else if(J + 2 < Body.size() && MatchWord(Body, J, "IF")) {
				I = J + 1;
				continue;
			} else if(J + 3 < Body.size() && MatchWord(Body, J, "LOOP")) {
				I = J + 3;
				continue;
			} else if(J + 3 < Body.size() && MatchWord(Body, J, "CASE")) {
				I = J + 3;
				continue;
			}
			if(St.BeginDepth > 0)
				--St.BeginDepth;
			I = J - 1;
			continue;
		}
		if(St.BeginDepth == 0 && St.IfDepth == 0 && St.LoopDepth == 0 && St.CaseDepth == 0) {
			if(MatchWord(Body, I, "IF") || MatchWord(Body, I, "CASE")) {
				throw std::runtime_error(Err::Prefixed(
				    "PROC", "Internal lowering error: IF/CASE reached linear scanner (control-flow extraction missed a block)."));
			}
			if(MatchWord(Body, I, "LOOP") || MatchWord(Body, I, "WHILE") || MatchWord(Body, I, "FOR")) {
				throw std::runtime_error(Err::Prefixed(
				    "PROC",
				    "PL/SQL or PL/pgSQL loops must be constant (WHILE FALSE/TRUE, FOR i IN 1..N with N<=32) for lowering."));
			}
		}
		if(MatchWord(Body, I, "IF"))
			++St.IfDepth;
		if(MatchWord(Body, I, "LOOP") || MatchWord(Body, I, "WHILE") || MatchWord(Body, I, "FOR"))
			++St.LoopDepth;
		if(MatchWord(Body, I, "CASE"))
			++St.CaseDepth;

		if(C == ';' && St.BeginDepth == 0 && St.IfDepth == 0 && St.LoopDepth == 0 && St.CaseDepth == 0) {
			std::string Rew = RewriteStatementForAstral(Cur, Dialect);
			if(!Rew.empty()) {
				if(!Out.empty())
					Out += '\n';
				Out += Rew;
				if(Out.back() != ';')
					Out += ';';
			}
			Cur.clear();
			continue;
		}
		Cur += C;
	}
	std::string Tail = RewriteStatementForAstral(Cur, Dialect);
	if(!Tail.empty()) {
		if(!Out.empty())
			Out += '\n';
		Out += Tail;
		if(Out.back() != ';')
			Out += ';';
	}
	return Out;
}

std::string FlattenLoweredLinear(const LoweredProcedureBody &Body) {
	std::string Out;
	for(const auto &Seg : Body.Segments) {
		if(!Seg.LinearSql.empty()) {
			if(!Out.empty())
				Out += '\n';
			Out += Seg.LinearSql;
		}
		for(const auto &Br : Seg.IfBranches) {
			for(const auto &Nested : Br.Segments) {
				if(!Nested.LinearSql.empty()) {
					if(!Out.empty())
						Out += '\n';
					Out += Nested.LinearSql;
				}
			}
		}
	}
	return Out;
}

LoweredProcedureBody LowerWithControlFlow(std::string_view BodyIn, ProcedureDialectKind Dialect) {
	LoweredProcedureBody Out;
	std::string Body = FoldConstantControlFlow(BodyIn);
	std::size_t Pos = 0;
	while(Pos < Body.size()) {
		const auto Ctrl = FindNextTopLevelIfOrCase(Body, Pos);
		ProcedureControlSegment Seg;
		const std::size_t LinearEnd = Ctrl ? *Ctrl : Body.size();
		Seg.LinearSql = LowerDialectBodyLinear(Body.substr(Pos, LinearEnd - Pos), Dialect, true);
		if(!Ctrl) {
			if(!Seg.LinearSql.empty() || Out.Segments.empty())
				Out.Segments.push_back(std::move(Seg));
			break;
		}
		IfBlockParsed Block;
		const bool Ok = MatchWord(Body, *Ctrl, "CASE") ? ParseCaseBlockAt(Body, *Ctrl, Block)
		                                               : ParseIfBlockAt(Body, *Ctrl, Block);
		if(!Ok)
			throw std::runtime_error(Err::Prefixed("PROC", "Failed to parse IF/CASE control block in procedure body."));
		for(auto &[Cond, Text] : Block.Branches) {
			ProcedureIfBranch Br;
			Br.ConditionSql = std::move(Cond);
			TrimInPlace(Br.ConditionSql);
			const LoweredProcedureBody Nested = LowerWithControlFlow(Text, Dialect);
			Br.Segments = std::move(Nested.Segments);
			Seg.IfBranches.push_back(std::move(Br));
		}
		if(!Block.ElseText.empty()) {
			ProcedureIfBranch ElseBr;
			const LoweredProcedureBody Nested = LowerWithControlFlow(Block.ElseText, Dialect);
			ElseBr.Segments = std::move(Nested.Segments);
			Seg.IfBranches.push_back(std::move(ElseBr));
		}
		Out.Segments.push_back(std::move(Seg));
		Pos = Block.ConsumedEnd;
	}
	Out.TryBodySql = FlattenLoweredLinear(Out);
	return Out;
}

} // namespace

struct ExceptionSplit {
	std::string_view TryPart;
	std::string_view ExceptionPart;
};

ExceptionSplit SplitExceptionSection(std::string_view Body) {
	const auto Ex = FindKeywordOutsideQuotes(Body, "EXCEPTION");
	if(!Ex)
		return {Body, {}};
	return {Body.substr(0, *Ex), Body.substr(*Ex + 9)};
}

std::vector<ProcedureExceptionWhen> ParseWhenHandlers(std::string_view ExceptionPart) {
	std::vector<ProcedureExceptionWhen> Out;
	std::size_t Pos = 0;
	if(MatchWord(ExceptionPart, 0, "EXCEPTION"))
		Pos = 9;
	while(Pos < ExceptionPart.size()) {
		while(Pos < ExceptionPart.size() && std::isspace(static_cast<unsigned char>(ExceptionPart[Pos])))
			++Pos;
		if(Pos >= ExceptionPart.size())
			break;
		if(MatchWord(ExceptionPart, Pos, "END")) {
			std::size_t J = Pos + 3;
			while(J < ExceptionPart.size() && std::isspace(static_cast<unsigned char>(ExceptionPart[J])))
				++J;
			if(J >= ExceptionPart.size() || !std::isalpha(static_cast<unsigned char>(ExceptionPart[J])))
				break;
		}
		if(!MatchWord(ExceptionPart, Pos, "WHEN"))
			break;
		Pos += 4;
		const std::size_t CondStart = Pos;
		while(Pos < ExceptionPart.size() && !MatchWord(ExceptionPart, Pos, "THEN"))
			++Pos;
		if(Pos >= ExceptionPart.size())
			throw std::runtime_error(Err::Prefixed("PROC", "EXCEPTION WHEN missing THEN."));
		std::string Cond(ExceptionPart.substr(CondStart, Pos - CondStart));
		while(!Cond.empty() && std::isspace(static_cast<unsigned char>(Cond.front())))
			Cond.erase(Cond.begin());
		while(!Cond.empty() && std::isspace(static_cast<unsigned char>(Cond.back())))
			Cond.pop_back();
		FoldAsciiUpper(Cond);
		if(Cond.rfind("SQLSTATE", 0) == 0) {
			const std::size_t Q0 = Cond.find('\'');
			if(Q0 != std::string::npos) {
				const std::size_t Q1 = Cond.find('\'', Q0 + 1);
				if(Q1 != std::string::npos)
					Cond = "SQLSTATE:" + Cond.substr(Q0 + 1, Q1 - Q0 - 1);
			}
		}
		Pos += 4;
		const std::size_t HandlerStart = Pos;
		int WhenDepth = 0;
		while(Pos < ExceptionPart.size()) {
			if(MatchWord(ExceptionPart, Pos, "WHEN") && WhenDepth == 0)
				break;
			if(MatchWord(ExceptionPart, Pos, "END") && WhenDepth == 0) {
				std::size_t J = Pos + 3;
				while(J < ExceptionPart.size() && std::isspace(static_cast<unsigned char>(ExceptionPart[J])))
					++J;
				if(J >= ExceptionPart.size() || !std::isalpha(static_cast<unsigned char>(ExceptionPart[J])))
					break;
			}
			if(MatchWord(ExceptionPart, Pos, "BEGIN"))
				++WhenDepth;
			if(MatchWord(ExceptionPart, Pos, "END") && WhenDepth > 0)
				--WhenDepth;
			++Pos;
		}
		std::string HandlerSql(ExceptionPart.substr(HandlerStart, Pos - HandlerStart));
		while(!HandlerSql.empty() && std::isspace(static_cast<unsigned char>(HandlerSql.front())))
			HandlerSql.erase(HandlerSql.begin());
		while(!HandlerSql.empty() && std::isspace(static_cast<unsigned char>(HandlerSql.back())))
			HandlerSql.pop_back();
		if(!Cond.empty() && !HandlerSql.empty())
			Out.push_back({std::move(Cond), std::move(HandlerSql)});
	}
	if(Out.empty() && !ExceptionPart.empty())
		throw std::runtime_error(Err::Prefixed("PROC", "EXCEPTION block has no WHEN handlers."));
	return Out;
}

LoweredProcedureBody LowerDialectBodyStructuredImpl(std::string_view BodyText, ProcedureDialectKind Dialect) {
	LoweredProcedureBody Out;
	std::string_view Body = BodyText;
	if(auto Block = ExtractBeginEndBody(Body, Dialect))
		Body = Block->Text;
	std::string Normalized;
	if(Dialect == ProcedureDialectKind::TSql) {
		Normalized = NormalizeTsqlControlFlow(std::string(Body));
		Body = Normalized;
	}
	const auto Split = SplitExceptionSection(Body);
	std::string_view TryRaw = Split.TryPart;
	StripDeclarePrefix(TryRaw);
	Out = LowerWithControlFlow(TryRaw, Dialect);
	if(!Split.ExceptionPart.empty()) {
		for(auto &H : ParseWhenHandlers(Split.ExceptionPart))
			Out.ExceptionHandlers.push_back({H.Condition, LowerWithControlFlow(H.HandlerSql, Dialect).TryBodySql});
	}
	return Out;
}

LoweredProcedureBody ProcedureParser::LowerDialectBodyStructured(std::string_view BodyText,
                                                                   ProcedureDialectKind Dialect) {
	return LowerDialectBodyStructuredImpl(BodyText, Dialect);
}

const char *ProcedureParser::DialectTag(ProcedureDialectKind Dialect) {
	switch(Dialect) {
	case ProcedureDialectKind::PlSql:
		return "plsql";
	case ProcedureDialectKind::PlPgSql:
		return "plpgsql";
	case ProcedureDialectKind::TSql:
		return "tsql";
	case ProcedureDialectKind::DuckDb:
		return "duckdb";
	default:
		return "standard";
	}
}

std::string NormalizeMacroBody(std::string_view Raw) {
	std::string Body(Raw);
	TrimInPlace(Body);
	while(!Body.empty() && Body.back() == ';')
		Body.pop_back();
	TrimInPlace(Body);
	if(Body.size() >= 2 && Body.front() == '(' && Body.back() == ')') {
		int Depth = 0;
		bool Wrapped = true;
		for(std::size_t I = 0; I < Body.size(); ++I) {
			if(Body[I] == '(')
				++Depth;
			else if(Body[I] == ')') {
				--Depth;
				if(Depth == 0 && I + 1 != Body.size())
					Wrapped = false;
			}
		}
		if(Wrapped && Depth == 0)
			Body = Body.substr(1, Body.size() - 2);
	}
	TrimInPlace(Body);
	return Body;
}

std::string UnquoteProcName(const ProcToken &Tok) {
	if(Tok.Kind == ProcTokKind::String && Tok.Text.size() >= 2 &&
	   (Tok.Text.front() == '\'' || Tok.Text.front() == '"'))
		return Tok.Text.substr(1, Tok.Text.size() - 2);
	return Tok.Text;
}

bool ProcedureParser::IsDialectProcedureStatement(std::string_view Source) {
	const FoldedDialectSource Folded(Source);
	if(Folded.Folded.find("CREATE") == std::string::npos)
		return false;
	if(Folded.Contains(" MACRO ") || Folded.Contains("CREATE MACRO"))
		return true;
	if(Folded.Contains("LANGUAGE SQL") || Folded.Contains("LANGUAGE 'SQL'") || Folded.Contains("LANGUAGE \"SQL\""))
		return true;
	const bool HasProcDecl = Folded.Folded.find("PROCEDURE") != std::string::npos ||
	                         Folded.Folded.find("FUNCTION") != std::string::npos || Folded.Contains(" PROC ") ||
	                         Folded.Contains("CREATE PROC");
	if(!HasProcDecl)
		return false;
	if(Folded.Folded.find(" AS (") != std::string::npos || Folded.Folded.find(" AS(") != std::string::npos)
		return false;
	if(Folded.Contains("OR REPLACE"))
		return true;
	if(Folded.Contains("OR ALTER"))
		return true;
	if(Folded.Contains("CREATE PROC ") || Folded.Contains("CREATE PROC\n") || Folded.Contains("CREATE PROC\t"))
		return true;
	if(SourceHasTsqlParameter(Source))
		return true;
	if(Folded.Contains("LANGUAGE") && Folded.Contains("PLPGSQL"))
		return true;
	if(Source.find("$$") != std::string::npos)
		return true;
	if(Source.find('$') != std::string::npos && Folded.Contains(" AS $"))
		return true;
	if(Folded.Contains(" IS") && Folded.Contains("BEGIN"))
		return true;
	if(Folded.Contains(" AS") && Folded.Contains("BEGIN") && Folded.Folded.find(" AS (") == std::string::npos)
		return true;
	return false;
}

std::string ProcedureParser::LowerDialectBody(std::string_view BodyText, ProcedureDialectKind Dialect) {
	return LowerWithControlFlow(BodyText, Dialect).TryBodySql;
}

ProcedureParser::ProcedureParser(std::string_view Source) : Source_(Source) {}

ProcedureParseResult ProcedureParser::ParseDialectCreate() const {
	if(Source_.size() > Limits::MaxSqlSourceBytes)
		throw std::runtime_error(Err::Prefixed("PROC", "Procedure statement exceeds size guard."));

	ProcLexer Lex(Source_);
	ProcedureParseResult Result;
	ProcedureDialectKind Dialect = ProcedureDialectKind::PlSql;
	if(HeaderLooksTsql(Source_))
		Dialect = ProcedureDialectKind::TSql;
	bool SawPlpgsql = false;
	bool SawLanguageSql = false;
	bool SawEmptyParams = false;
	bool IsMacro = false;

	auto RequireKw = [&](const char *Kw) {
		ProcToken T = Lex.Next();
		if(!KeywordIs(T, Kw))
			throw std::runtime_error(
			    Err::FormatSqlParse(std::string("Expected ") + Kw + " in dialect CREATE statement.", Source_,
			                        T.Begin, T.Text));
	};

	RequireKw("CREATE");
	ProcToken Second = Lex.Next();
	if(KeywordIs(Second, "OR")) {
		ProcToken Third = Lex.Next();
		if(KeywordIs(Third, "ALTER")) {
			Result.OrReplace = true;
			Dialect = ProcedureDialectKind::TSql;
			Second = Lex.Next();
		} else if(KeywordIs(Third, "REPLACE")) {
			Result.OrReplace = true;
			Second = Lex.Next();
		} else {
			throw std::runtime_error(Err::FormatSqlParse("Expected ALTER or REPLACE after CREATE OR.", Source_,
			                                             Third.Begin, Third.Text));
		}
	}

	if(KeywordIs(Second, "MACRO")) {
		IsMacro = true;
		Dialect = ProcedureDialectKind::DuckDb;
	} else if(!KeywordIs(Second, "FUNCTION") && !KeywordIs(Second, "PROCEDURE") && !KeywordIs(Second, "PROC"))
		throw std::runtime_error(Err::FormatSqlParse("Expected PROCEDURE, PROC, FUNCTION, or MACRO after CREATE.",
		                                             Source_, Second.Begin, Second.Text));
	if(KeywordIs(Second, "PROC"))
		Dialect = ProcedureDialectKind::TSql;
	if(KeywordIs(Second, "FUNCTION"))
		Dialect = ProcedureDialectKind::PlPgSql;

	std::optional<ProcToken> Pending;
	ProcToken NameTok = Lex.Next();
	if(!IdentIs(NameTok) && NameTok.Kind != ProcTokKind::String)
		throw std::runtime_error(
		    Err::FormatSqlParse("Expected procedure name.", Source_, NameTok.Begin, NameTok.Text));
	Result.ProcedureName = UnquoteProcName(NameTok);
	if(IdentIs(NameTok)) {
		ProcToken Qual = Lex.Next();
		if(Qual.Kind == ProcTokKind::Punct && Qual.Text == ".") {
			ProcToken QualName = Lex.Next();
			if(!IdentIs(QualName) && QualName.Kind != ProcTokKind::String)
				throw std::runtime_error(Err::FormatSqlParse("Expected schema-qualified procedure name.", Source_,
				                                             QualName.Begin, QualName.Text));
			Result.ProcedureName = UnquoteProcName(QualName);
		} else if(Qual.Kind == ProcTokKind::Punct && Qual.Text == "(") {
			ProcToken Close = Lex.Next();
			if(Close.Kind == ProcTokKind::Punct && Close.Text == ")")
				SawEmptyParams = true;
			else {
				Pending = Close;
				SkipBalancedParens(Lex);
			}
		} else {
			Pending = Qual;
		}
	}

	if(!Pending.has_value()) {
		ProcToken AfterName = Lex.Next();
		if(AfterName.Kind == ProcTokKind::Punct && AfterName.Text == "(") {
			ProcToken Close = Lex.Next();
			if(Close.Kind == ProcTokKind::Punct && Close.Text == ")")
				SawEmptyParams = true;
			else {
				Pending = Close;
				SkipBalancedParens(Lex);
			}
		} else
			Pending = AfterName;
	}

	for(;;) {
		ProcToken T = Pending ? std::exchange(Pending, std::nullopt).value() : Lex.Next();
		if(T.Kind == ProcTokKind::End)
			break;
		if(KeywordIs(T, "IF")) {
			RequireKw("NOT");
			RequireKw("EXISTS");
			Result.IfNotExists = true;
			continue;
		}
		if(T.Kind == ProcTokKind::Punct && T.Text == "@") {
			for(;;) {
				ProcToken Skip = Lex.Next();
				if(Skip.Kind == ProcTokKind::End)
					break;
				if(KeywordIs(Skip, "AS") || KeywordIs(Skip, "IS")) {
					Pending = Skip;
					break;
				}
				if(KeywordIs(Skip, "WITH") || KeywordIs(Skip, "RETURNS") || KeywordIs(Skip, "RETURN"))
					Pending = Skip;
			}
			Dialect = ProcedureDialectKind::TSql;
			continue;
		}
		if(IdentIs(T) && !T.Text.empty() && T.Text[0] == '@') {
			for(;;) {
				ProcToken Skip = Lex.Next();
				if(Skip.Kind == ProcTokKind::End)
					break;
				if(KeywordIs(Skip, "AS") || KeywordIs(Skip, "IS")) {
					Pending = Skip;
					break;
				}
				if(Skip.Kind == ProcTokKind::Punct && Skip.Text == ",")
					continue;
				if(KeywordIs(Skip, "WITH") || KeywordIs(Skip, "RETURNS") || KeywordIs(Skip, "RETURN"))
					Pending = Skip;
			}
			Dialect = ProcedureDialectKind::TSql;
			continue;
		}
		if(KeywordIs(T, "WITH")) {
			ProcToken Next = Lex.Next();
			if(Next.Kind == ProcTokKind::Punct && Next.Text == "(")
				SkipBalancedParens(Lex);
			else
				Pending = Next;
			continue;
		}
		if(KeywordIs(T, "AUTHID")) {
			(void)Lex.Next();
			continue;
		}
		if(KeywordIs(T, "VOLATILE") || KeywordIs(T, "STABLE") || KeywordIs(T, "IMMUTABLE") || KeywordIs(T, "STRICT") ||
		   KeywordIs(T, "LEAKPROOF") || KeywordIs(T, "DEFINER") || KeywordIs(T, "INVOKER"))
			continue;
		if(KeywordIs(T, "SECURITY")) {
			(void)Lex.Next();
			continue;
		}
		if(KeywordIs(T, "COST") || KeywordIs(T, "PARALLEL")) {
			(void)Lex.Next();
			continue;
		}
		if(KeywordIs(T, "LANGUAGE")) {
			ProcToken Lang = Lex.Next();
			std::string LangFold = Lang.Text;
			if(Lang.Kind == ProcTokKind::String && LangFold.size() >= 2)
				LangFold = LangFold.substr(1, LangFold.size() - 2);
			FoldAsciiUpper(LangFold);
			if(LangFold == "PLPGSQL") {
				SawPlpgsql = true;
				Dialect = ProcedureDialectKind::PlPgSql;
			} else if(LangFold == "SQL") {
				SawLanguageSql = true;
				Dialect = ProcedureDialectKind::PlPgSql;
			}
			continue;
		}
		if(KeywordIs(T, "RETURNS") || KeywordIs(T, "RETURN")) {
			int Depth = 0;
			for(;;) {
				ProcToken R = Lex.Next();
				if(R.Kind == ProcTokKind::End)
					break;
				if(R.Kind == ProcTokKind::Punct && R.Text == "(")
					++Depth;
				else if(R.Kind == ProcTokKind::Punct && R.Text == ")") {
					if(Depth == 0)
						break;
					--Depth;
				} else if(Depth == 0 && (KeywordIs(R, "LANGUAGE") || KeywordIs(R, "AS") || KeywordIs(R, "IS"))) {
					Pending = R;
					break;
				}
			}
			continue;
		}
		if(KeywordIs(T, "AS") || KeywordIs(T, "IS")) {
			const std::size_t BodyStart = Lex.Position();
			if(IsMacro) {
				std::string_view MacroBody = Source_.substr(BodyStart);
				while(!MacroBody.empty() && std::isspace(static_cast<unsigned char>(MacroBody.back())))
					MacroBody.remove_suffix(1);
				if(!MacroBody.empty() && MacroBody.back() == ';')
					MacroBody.remove_suffix(1);
				const std::string Normalized = NormalizeMacroBody(MacroBody);
				Result.Dialect = ProcedureDialectKind::DuckDb;
				Result.DialectTag = DialectTag(Result.Dialect);
				Result.LoweredBodySql = LowerDialectBody(Normalized, Result.Dialect);
				Result.Body_.TryBodySql = Result.LoweredBodySql;
				return Result;
			}
			if(KeywordIs(T, "IS") && !SourceHasTsqlParameter(Source_))
				Dialect = ProcedureDialectKind::PlSql;
			else if(KeywordIs(T, "AS") && HeaderLooksTsql(Source_))
				Dialect = ProcedureDialectKind::TSql;
			if(SawPlpgsql)
				Dialect = ProcedureDialectKind::PlPgSql;
			if(Result.OrReplace && SawEmptyParams && !SawPlpgsql && !SawLanguageSql &&
			   Dialect != ProcedureDialectKind::TSql && !SourceHasTsqlParameter(Source_))
				Dialect = ProcedureDialectKind::DuckDb;
			if(auto Dollar = ExtractDollarBody(Source_, BodyStart)) {
				Result.Dialect = Dialect;
				if(SawLanguageSql)
					Result.DialectTag = "sql";
				else
					Result.DialectTag = DialectTag(Result.Dialect);
				if(SawLanguageSql && !SourceContains(Dollar->Text, "BEGIN"))
					Result.LoweredBodySql = LowerDialectBody(Dollar->Text, Result.Dialect);
				else {
					Result.Body_ = LowerDialectBodyStructured(Dollar->Text, Result.Dialect);
					Result.LoweredBodySql = Result.Body_.TryBodySql;
				}
				return Result;
			}
			const std::string_view Tail = Source_.substr(BodyStart);
			if(auto Block = ExtractBeginEndBody(Tail, Dialect)) {
				Result.Dialect = Dialect;
				if(SawLanguageSql)
					Result.DialectTag = "sql";
				else
					Result.DialectTag = DialectTag(Result.Dialect);
				Result.Body_ = LowerDialectBodyStructured(Block->Text, Result.Dialect);
				Result.LoweredBodySql = Result.Body_.TryBodySql;
				return Result;
			}
			throw std::runtime_error(Err::FormatSqlParse(
			    "Expected dollar-quoted body or BEGIN … END block after AS/IS.", Source_, BodyStart));
		}
	}
	throw std::runtime_error(
	    Err::FormatSqlParse("Incomplete dialect CREATE PROCEDURE/FUNCTION header (missing AS/IS body).", Source_, 0));
}

} // namespace AstralDB::SQL
