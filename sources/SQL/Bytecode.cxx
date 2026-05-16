#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
#include <SQL/SetExprEval.hxx>
#include <SQL/SQL.hxx>
#include <IO/Limits.hxx>
#include <IO/Error.hxx>
#include <Database/Database.hxx>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <functional>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <unordered_set>
#include <optional>
#include <sstream>
#include <string_view>
#include <new>
#include <cctype>
#include <climits>
#include <cmath>
#include <numeric>
#include <fstream>
#include <limits>
#include <set>

namespace AstralDB {
namespace SQL {

namespace {

[[noreturn]] inline void FailVm(std::string Message) {
	throw std::runtime_error(AstralDB::Err::Prefixed("SQL VM", std::move(Message)));
}

static std::string *AllocateVmImmediateString(const std::string &S) {
	try {
		return new std::string(S);
	} catch(const std::bad_alloc &) {
		FailVm("Out of memory while allocating a string literal in the SQL virtual machine.");
	}
}

int CompareScalars(const std::string &a, const std::string &b) {
	size_t posA = 0, posB = 0;
	try {
		const long long ia = std::stoll(a, &posA);
		const long long ib = std::stoll(b, &posB);
		if(posA == a.size() && posB == b.size()) {
			if(ia < ib)
				return -1;
			if(ia > ib)
				return 1;
			return 0;
		}
	} catch(...) {
	}
	if(a < b)
		return -1;
	if(a > b)
		return 1;
	return 0;
}

bool CellCompare(const std::string &lhs, const std::string &rhs, const std::string &op) {
	const int c = CompareScalars(lhs, rhs);
	if(op == "=" || op == "==")
		return c == 0;
	if(op == "!=")
		return c != 0;
	if(op == "<")
		return c < 0;
	if(op == "<=")
		return c <= 0;
	if(op == ">")
		return c > 0;
	if(op == ">=")
		return c >= 0;
	return false;
}

static constexpr const char *kOpIsNull = "__IS_NULL__";
static constexpr const char *kOpIsNotNull = "__IS_NOT_NULL__";
static constexpr const char *kOpIn = "__IN__";

static bool CellIsSqlNull(const Database::Item &Row, const std::string &Col) {
	auto It = Row.find(Col);
	return It == Row.end() || It->second.empty();
}

static bool SqlLike(const std::string &Str, const std::string &Pat) {
	const size_t n = Str.size(), m = Pat.size();
	const size_t Rows = m + 1;
	const size_t Cols = n + 1;
	if(Rows != 0 && Cols != 0) {
		if(Rows > Limits::MaxSqlLikeDpCells || Cols > Limits::MaxSqlLikeDpCells)
			return false;
		if(Rows > Limits::MaxSqlLikeDpCells / Cols)
			return false;
	}
	std::vector<std::vector<char>> Dp(m + 1, std::vector<char>(n + 1, 0));
	Dp[0][0] = 1;
	for(size_t I = 1; I <= m; ++I) {
		const char Pc = Pat[I - 1];
		for(size_t J = 0; J <= n; ++J) {
			if(Pc == '%')
				Dp[I][J] = Dp[I - 1][J] || (J > 0 && Dp[I][J - 1]);
			else if(Pc == '_')
				Dp[I][J] = J > 0 && Dp[I - 1][J - 1];
			else
				Dp[I][J] = J > 0 && Dp[I - 1][J - 1] && Str[J - 1] == Pc;
		}
	}
	return Dp[m][n];
}

static bool SplitInList(const std::string &Blob, std::vector<std::string> &OutVals) {
	OutVals.clear();
	size_t Start = 0;
	for(size_t K = 0; K < Blob.size(); ++K) {
		if(Blob[K] == '\x1E') {
			OutVals.emplace_back(Blob.substr(Start, K - Start));
			Start = K + 1;
		}
	}
	OutVals.emplace_back(Blob.substr(Start));
	return !OutVals.empty();
}

using RowTriple = std::tuple<std::string, std::string, std::string>;

static constexpr char kExistPredColBytecode[] = "__ASTRAL_EXISTS__";
static constexpr char kAstRhsColMarker[] = "__AST_RHS_COL__:";

/** Correlate enclosing row with inner; only overwrite with inner cells that belong to the inner relation's schema so
 *  stray/synthetic keys cannot shadow outer columns during correlation. Caller must obey \c Database::TableSchemaAssumeDbMutexHeld rules. */
static Database::Item MergeForExistsRow(const Database *Db, const std::string &InnerRelation,
                                          const Database::Item &Enclosing, const Database::Item &InnerRow) {
	Database::Item Out = Enclosing;
	if(!Db) {
		FailVm("INTERNAL: EXISTS merge missing Database pointer");
	}
	const auto Sch = Db->TableSchemaAssumeDbMutexHeld(InnerRelation);
	if(!Sch.has_value()) {
		for(const auto &KV : InnerRow)
			Out[KV.first] = KV.second;
		return Out;
	}
	std::unordered_set<std::string> AllowedCols;
	for(const auto &Col : *Sch)
		AllowedCols.insert(Col.Name);
	for(const auto &KV : InnerRow) {
		if(AllowedCols.count(KV.first))
			Out[KV.first] = KV.second;
	}
	return Out;
}

static bool PullLeI64(const std::string &V, size_t &Off, int64_t &Out) {
	if(Off + 8 > V.size())
		return false;
	uint64_t U = 0;
	for(int B = 0; B < 8; ++B)
		U |= static_cast<uint64_t>(static_cast<unsigned char>(V[Off + B])) << (8 * B);
	Off += 8;
	Out = static_cast<int64_t>(U);
	return true;
}

static bool PullSizedString(const std::string &V, size_t &Off, std::string &Out) {
	int64_t Len = 0;
	if(!PullLeI64(V, Off, Len) || Len < 0 || static_cast<size_t>(Len) > V.size() - Off)
		return false;
	Out.assign(V.data() + Off, static_cast<size_t>(Len));
	Off += static_cast<size_t>(Len);
	return true;
}

/** Mirror \c PackDnfOperandsBlob in Codegen.cxx: alternating 'Q'+le64 / 'S'+len prefixed strings. */
static bool DecodePackedDnfOperands(const std::string &Blob, size_t StartOff, size_t EndOff,
                                    std::vector<std::vector<RowTriple>> &Branches) {
	Branches.clear();
	size_t Off = StartOff;
	if(Off >= EndOff)
		return false;
	int64_t Nb = 0;
	if(Blob[Off++] != 'Q' || !PullLeI64(Blob, Off, Nb) || Nb < 0 || Nb > 64)
		return false;
	for(int64_t B = 0; B < Nb; ++B) {
		if(Off >= EndOff || Blob[Off++] != 'Q')
			return false;
		int64_t Nk = 0;
		if(!PullLeI64(Blob, Off, Nk) || Nk < 0 || Nk > 4096)
			return false;
		std::vector<RowTriple> Conj;
		for(int64_t K = 0; K < Nk; ++K) {
			std::string Cs, Os, Vs;
			if(Off >= EndOff || Blob[Off++] != 'S' || !PullSizedString(Blob, Off, Cs))
				return false;
			if(Off >= EndOff || Blob[Off++] != 'S' || !PullSizedString(Blob, Off, Os))
				return false;
			if(Off >= EndOff || Blob[Off++] != 'S' || !PullSizedString(Blob, Off, Vs))
				return false;
			Conj.emplace_back(std::move(Cs), std::move(Os), std::move(Vs));
		}
		Branches.push_back(std::move(Conj));
	}
	return Off == EndOff;
}

static bool DecodeExistPayload(const std::string &Rhs, bool &NegOut, std::vector<std::vector<RowTriple>> &Inner) {
	const size_t Split = Rhs.find('\x1E');
	if(Split == std::string::npos || Split == 0 || (Rhs[0] != '0' && Rhs[0] != '1'))
		return false;
	NegOut = Rhs[0] == '1';
	return DecodePackedDnfOperands(Rhs, Split + 1, Rhs.size(), Inner);
}

static bool MatchWhereDnf(const Database *Db, const Database::Item &Row,
                          const std::vector<std::vector<RowTriple>> &Dnf);

static bool ExistPredicateHolds(const Database *Db, const RowTriple &Pred, const Database::Item &EnclosingRow) {
	const auto &[Col, Op, Rhs] = Pred;
	(void)Col;
	if(!Db)
		FailVm("INTERNAL: EXISTS evaluation requires Database context");
	std::vector<std::vector<RowTriple>> Inner;
	bool Neg = false;
	if(!DecodeExistPayload(Rhs, Neg, Inner))
		return Neg;
	const auto Tit = Db->Tables_.find(Op);
	if(Tit == Db->Tables_.end())
		return Neg;
	bool Any = false;
	for(const auto &InnerRow : Tit->second) {
		const Database::Item Combined = MergeForExistsRow(Db, Op, EnclosingRow, InnerRow);
		if(MatchWhereDnf(Db, Combined, Inner)) {
			Any = true;
			break;
		}
	}
	return Neg ? !Any : Any;
}

static bool MatchOnePredicate(const Database *Db, const Database::Item &Row, const RowTriple &Pred) {
	const auto &[Col, Op, Rhs] = Pred;
	if(Col == kExistPredColBytecode)
		return ExistPredicateHolds(Db, Pred, Row);
	auto ItCol = Row.find(Col);
	const std::string Lhs = ItCol == Row.end() ? "" : ItCol->second;

	static constexpr size_t kRhsColMarkLen = sizeof(kAstRhsColMarker) - 1;
	if(Op != kOpIsNull && Op != kOpIsNotNull && Op != kOpIn && Rhs.size() >= kRhsColMarkLen &&
	   Rhs.compare(0, kRhsColMarkLen, kAstRhsColMarker, kRhsColMarkLen) == 0) {
		const std::string_view RcolSv(Rhs.data() + kRhsColMarkLen, Rhs.size() - kRhsColMarkLen);
		const std::string Rcol(RcolSv.begin(), RcolSv.end());
		auto ItR = Row.find(Rcol);
		const std::string RhsVal = ItR == Row.end() ? "" : ItR->second;
		if(Op == "LIKE")
			return SqlLike(Lhs, RhsVal);
		if(Op == "NOT LIKE")
			return !SqlLike(Lhs, RhsVal);
		return CellCompare(Lhs, RhsVal, Op);
	}

	if(Op == kOpIsNull)
		return CellIsSqlNull(Row, Col);
	if(Op == kOpIsNotNull)
		return !CellIsSqlNull(Row, Col);
	if(Op == kOpIn) {
		std::vector<std::string> Vals;
		if(!SplitInList(Rhs, Vals))
			return false;
		for(const auto &V : Vals) {
			if(CellCompare(Lhs, V, "="))
				return true;
		}
		return false;
	}
	if(Op == "LIKE")
		return SqlLike(Lhs, Rhs);
	if(Op == "NOT LIKE")
		return !SqlLike(Lhs, Rhs);
	return CellCompare(Lhs, Rhs, Op);
}

static bool MatchBranchConjunction(const Database *Db, const Database::Item &Row, const std::vector<RowTriple> &Branch) {
	for(const auto &P : Branch) {
		if(!MatchOnePredicate(Db, Row, P))
			return false;
	}
	return true;
}

static bool MatchWhereDnf(const Database *Db, const Database::Item &Row,
                          const std::vector<std::vector<RowTriple>> &Dnf) {
	for(const auto &B : Dnf) {
		if(B.empty()) {
			/* Empty conjunction is tautological AND. As an OR-sum term, honor that only when
			   the predicate is exactly one tautology branch (inner WHERE omitted in EXISTS).
			   Extra empty terms from flatten/codegen skew must not swallow the whole disjunction.
			 */
			if(Dnf.size() == 1)
				return true;
			continue;
		}
		if(MatchBranchConjunction(Db, Row, B))
			return true;
	}
	return false;
}

static bool ReadDnfOperands(const std::vector<Value> &Ops, size_t Start, size_t &OutEnd,
                             std::vector<std::vector<RowTriple>> &OutBranches) {
	size_t I = Start;
	if(I >= Ops.size())
		return false;
	const auto *Nb = std::get_if<int64_t>(&Ops[I++]);
	if(!Nb || *Nb < 0 || *Nb > 64)
		return false;
	for(int64_t B = 0; B < *Nb; ++B) {
		if(I >= Ops.size())
			return false;
		const auto *Nk = std::get_if<int64_t>(&Ops[I++]);
		if(!Nk || *Nk < 0 || *Nk > 128)
			return false;
		std::vector<RowTriple> Branch;
		for(int64_t K = 0; K < *Nk; ++K) {
			if(I + 3 > Ops.size())
				return false;
			const auto *Cs = std::get_if<std::string>(&Ops[I++]);
			const auto *Os = std::get_if<std::string>(&Ops[I++]);
			const auto *Vs = std::get_if<std::string>(&Ops[I++]);
			if(!Cs || !Os || !Vs)
				return false;
			Branch.emplace_back(*Cs, *Os, *Vs);
		}
		OutBranches.push_back(std::move(Branch));
	}
	OutEnd = I;
	return true;
}

static int64_t ReadLeI64FromBlob(const std::string &B, size_t &Pos) {
	if(Pos + 8 > B.size())
		FailVm("DNF blob: truncated integer");
	uint64_t U = 0;
	for(int J = 0; J < 8; ++J)
		U |= static_cast<uint64_t>(static_cast<unsigned char>(B[Pos + J])) << (8 * J);
	Pos += 8;
	return static_cast<int64_t>(U);
}

/** Rebuild FILTER_Dnf-style branch list produced by Codegen \c PackDnfOperandsBlob. */
static bool UnpackDnfBlobToBranches(const std::string &Blob, std::vector<std::vector<RowTriple>> &OutBranches) {
	OutBranches.clear();
	std::vector<Value> Tmp;
	size_t Pos = 0;
	while(Pos < Blob.size()) {
		const unsigned char Tag = static_cast<unsigned char>(Blob[Pos++]);
		if(Tag == static_cast<unsigned char>('Q'))
			Tmp.push_back(ReadLeI64FromBlob(Blob, Pos));
		else if(Tag == static_cast<unsigned char>('S')) {
			const int64_t Len = ReadLeI64FromBlob(Blob, Pos);
			if(Len < 0 || static_cast<size_t>(Len) > Blob.size() - Pos)
				return false;
			Tmp.emplace_back(Blob.substr(Pos, static_cast<size_t>(Len)));
			Pos += static_cast<size_t>(Len);
		} else
			return false;
	}
	size_t End = 0;
	if(!ReadDnfOperands(Tmp, 0, End, OutBranches))
		return false;
	return End == Tmp.size();
}

static bool EvalPackedWhereDnfLocal(const Database *Db, const Database::Item &Row,
                                    std::string_view PackedDnfBlob) {
	std::string Blob(PackedDnfBlob);
	std::vector<std::vector<RowTriple>> Branches;
	if(!UnpackDnfBlobToBranches(Blob, Branches))
		return false;
	return MatchWhereDnf(Db, Row, Branches);
}

static void AssignCaseOutputColumn(Database::Item &Row, const std::string &OutCol, int64_t Kind,
                                  const std::string &Payload) {
	if(Kind == 3)
		FailVm("CASE_EVAL: reserved scalar opcode in value position");
	if(Kind == 2)
		Row.erase(OutCol);
	else if(Kind == 1) {
		auto It = Row.find(Payload);
		if(It == Row.end())
			Row.erase(OutCol);
		else
			Row[OutCol] = It->second;
	} 	else
		Row[OutCol] = Payload;
}

static std::optional<std::string> ReadCastProjectionSource(const Database::Item &Row, int64_t SrcKind,
                                                           const std::string &Payload) {
	if(SrcKind == 2)
		return std::nullopt;
	if(SrcKind == 0)
		return Payload;
	if(SrcKind != 1)
		FailVm("CAST_EVAL: bad source operand kind");
	auto It = Row.find(Payload);
	if(It == Row.end() || It->second.empty())
		return std::nullopt;
	return It->second;
}

static std::optional<int> ParseIsoYmd(const std::string &S) {
	if(S.size() < 10 || S[4] != '-' || S[7] != '-')
		return std::nullopt;
	auto Digit = [](char C) { return C >= '0' && C <= '9'; };
	for(int I : {0, 1, 2, 3, 5, 6, 8, 9})
		if(!Digit(S[static_cast<size_t>(I)]))
			return std::nullopt;
	const int Y = (S[0] - '0') * 1000 + (S[1] - '0') * 100 + (S[2] - '0') * 10 + (S[3] - '0');
	const int M = (S[5] - '0') * 10 + (S[6] - '0');
	const int D = (S[8] - '0') * 10 + (S[9] - '0');
	if(M < 1 || M > 12 || D < 1 || D > 31)
		return std::nullopt;
	return (Y * 10000) + (M * 100) + D;
}

static std::string FormatIsoYmd(int Packed) {
	const int Y = Packed / 10000;
	const int M = (Packed / 100) % 100;
	const int D = Packed % 100;
	std::ostringstream O;
	O << Y << '-' << (M < 10 ? "0" : "") << M << '-' << (D < 10 ? "0" : "") << D;
	return std::move(O).str();
}

static bool IsLeap(int Y) { return (Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0); }

static int DaysInMonth(int Y, int M) {
	static const int Dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if(M < 1 || M > 12)
		return 0;
	return Dm[M] + (M == 2 && IsLeap(Y) ? 1 : 0);
}

static std::optional<int> IsoAddDays(const std::string &Iso, int Delta) {
	const auto Packed = ParseIsoYmd(Iso);
	if(!Packed)
		return std::nullopt;
	int Y = *Packed / 10000;
	int M = (*Packed / 100) % 100;
	int D = *Packed % 100;
	int Rem = Delta;
	while(Rem != 0) {
		if(Rem > 0) {
			const int Dim = DaysInMonth(Y, M);
			const int Left = Dim - D;
			if(Rem <= Left) {
				D += Rem;
				Rem = 0;
			} else {
				Rem -= Left + 1;
				D = 1;
				++M;
				if(M > 12) {
					M = 1;
					++Y;
				}
			}
		} else {
			if(D + Rem >= 1) {
				D += Rem;
				Rem = 0;
			} else {
				Rem += D;
				--M;
				if(M < 1) {
					M = 12;
					--Y;
				}
				D = DaysInMonth(Y, M);
			}
		}
	}
	return Y * 10000 + M * 100 + D;
}

static std::optional<std::string> EvalSqlSubstringCells(const std::vector<std::string> &Cells) {
	if(Cells.empty() || Cells[0].empty())
		return std::string();
	const std::string &S = Cells[0];
	if(Cells.size() == 1)
		return S;
	long long Start = 1;
	try {
		Start = std::stoll(Cells[1]);
	} catch(...) {
		return std::nullopt;
	}
	if(Start < 1)
		return std::string();
	const size_t Off = static_cast<size_t>(Start - 1);
	if(Off >= S.size())
		return std::string();
	if(Cells.size() == 2)
		return S.substr(Off);
	long long Len = 0;
	try {
		Len = std::stoll(Cells[2]);
	} catch(...) {
		return std::nullopt;
	}
	if(Len < 0)
		return std::string();
	return S.substr(Off, static_cast<size_t>(Len));
}

static std::optional<std::string> EvalScalarSqlFn(ScalarSqlFn Fn, const std::vector<std::string> &Cells,
                                                  const Database::Item &Row) {
	switch(Fn) {
	case ScalarSqlFn::Upper: {
		if(Cells.size() != 1)
			return std::nullopt;
		std::string O;
		O.reserve(Cells[0].size());
		for(unsigned char C : Cells[0])
			O.push_back(static_cast<char>(std::toupper(C)));
		return O;
	}
	case ScalarSqlFn::Lower: {
		if(Cells.size() != 1)
			return std::nullopt;
		std::string O;
		O.reserve(Cells[0].size());
		for(unsigned char C : Cells[0])
			O.push_back(static_cast<char>(std::tolower(C)));
		return O;
	}
	case ScalarSqlFn::CharLength:
		if(Cells.size() != 1)
			return std::nullopt;
		return std::to_string(Cells[0].size());
	case ScalarSqlFn::SubstringFromFor:
		return EvalSqlSubstringCells(Cells);
	case ScalarSqlFn::PositionIn: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Pos = Cells[1].find(Cells[0]);
		if(Pos == std::string::npos)
			return std::string("0");
		return std::to_string(static_cast<unsigned long long>(Pos + 1));
	}
	case ScalarSqlFn::TrimBoth:
	case ScalarSqlFn::TrimLeading:
	case ScalarSqlFn::TrimTrailing: {
		if(Cells.size() != 1)
			return std::nullopt;
		std::string_view V = Cells[0];
		const auto IsSpace = [](char C) { return std::isspace(static_cast<unsigned char>(C)); };
		if(Fn == ScalarSqlFn::TrimBoth || Fn == ScalarSqlFn::TrimLeading) {
			while(!V.empty() && IsSpace(V.front()))
				V.remove_prefix(1);
		}
		if(Fn == ScalarSqlFn::TrimBoth || Fn == ScalarSqlFn::TrimTrailing) {
			while(!V.empty() && IsSpace(V.back()))
				V.remove_suffix(1);
		}
		return std::string(V);
	}
	case ScalarSqlFn::ConcatVariadic: {
		if(Cells.size() < 2)
			return std::nullopt;
		std::string O;
		for(const auto &C : Cells)
			O += C;
		return O;
	}
	case ScalarSqlFn::ExtractYear:
	case ScalarSqlFn::ExtractMonth:
	case ScalarSqlFn::ExtractDay: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto P = ParseIsoYmd(Cells[0]);
		if(!P)
			return std::nullopt;
		if(Fn == ScalarSqlFn::ExtractYear)
			return std::to_string(*P / 10000);
		if(Fn == ScalarSqlFn::ExtractMonth)
			return std::to_string((*P / 100) % 100);
		return std::to_string(*P % 100);
	}
	case ScalarSqlFn::DateAddDays: {
		if(Cells.size() != 2)
			return std::nullopt;
		int Delta = 0;
		try {
			Delta = static_cast<int>(std::stoll(Cells[1]));
		} catch(...) {
			return std::nullopt;
		}
		const auto Out = IsoAddDays(Cells[0], Delta);
		return Out ? std::optional<std::string>(FormatIsoYmd(*Out)) : std::nullopt;
	}
	case ScalarSqlFn::DateSubDays: {
		if(Cells.size() != 2)
			return std::nullopt;
		int Delta = 0;
		try {
			Delta = static_cast<int>(std::stoll(Cells[1]));
		} catch(...) {
			return std::nullopt;
		}
		const auto Out = IsoAddDays(Cells[0], -Delta);
		return Out ? std::optional<std::string>(FormatIsoYmd(*Out)) : std::nullopt;
	}
	case ScalarSqlFn::DateDiffDays: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto A = ParseIsoYmd(Cells[0]);
		const auto B = ParseIsoYmd(Cells[1]);
		if(!A || !B)
			return std::nullopt;
		int Y1 = *A / 10000, M1 = (*A / 100) % 100, D1 = *A % 100;
		int Y2 = *B / 10000, M2 = (*B / 100) % 100, D2 = *B % 100;
		auto ToDays = [](int Y, int M, int D) {
			long long Days = static_cast<long long>(D - 1);
			for(int Yr = 1; Yr < Y; ++Yr)
				Days += IsLeap(Yr) ? 366 : 365;
			for(int Mo = 1; Mo < M; ++Mo)
				Days += DaysInMonth(Y, Mo);
			return Days;
		};
		return std::to_string(ToDays(Y2, M2, D2) - ToDays(Y1, M1, D1));
	}
	case ScalarSqlFn::Grouping: {
		if(Cells.size() != 1 || Cells[0].empty())
			return std::nullopt;
		const std::string Key = "_grouping_" + Cells[0];
		auto It = Row.find(Key);
		if(It == Row.end())
			return std::string("0");
		return It->second;
	}
	default:
		return std::nullopt;
	}
}

static std::optional<std::string> SqlApplyCast(const std::optional<std::string> &InOpt, SqlCastTarget T) {
	if(!InOpt.has_value())
		return std::nullopt;
	std::string_view V = std::string_view(*InOpt);
	size_t L = 0;
	size_t R = V.size();
	while(L < R && std::isspace(static_cast<unsigned char>(V[L])))
		++L;
	while(R > L && std::isspace(static_cast<unsigned char>(V[R - 1])))
		--R;
	const std::string_view Trim = V.substr(L, R - L);

	switch(T) {
	case SqlCastTarget::Text:
		return std::string(Trim);
	case SqlCastTarget::Integer: {
		if(Trim.empty())
			return std::nullopt;
		try {
			const std::string S(Trim);
			if(S.find_first_of(".eE") != std::string::npos) {
				const double D = std::stod(S);
				const double Tr = std::trunc(D);
				if(Tr > static_cast<double>(LLONG_MAX) || Tr < static_cast<double>(LLONG_MIN))
					return std::nullopt;
				return std::to_string(static_cast<long long>(Tr));
			}
			return std::to_string(std::stoll(S));
		} catch(...) {
			return std::nullopt;
		}
	}
	case SqlCastTarget::Real: {
		if(Trim.empty())
			return std::nullopt;
		try {
			const double D = std::stod(std::string(Trim));
			std::ostringstream Os;
			Os << D;
			return std::move(Os).str();
		} catch(...) {
			return std::nullopt;
		}
	}
	case SqlCastTarget::Boolean: {
		if(Trim.empty())
			return std::nullopt;
		const std::string S(Trim);
		std::string U;
		U.reserve(S.size());
		for(unsigned char C : S)
			U.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(C))));
		if(U == "TRUE" || U == "T" || U == "YES" || U == "Y" || U == "ON" || U == "1")
			return std::string("1");
		if(U == "FALSE" || U == "F" || U == "NO" || U == "N" || U == "OFF" || U == "0")
			return std::string("0");
		try {
			const double D = std::stod(std::string(Trim));
			return std::string(D != 0.0 ? "1" : "0");
		} catch(...) {
			return std::nullopt;
		}
	}
	default:
		FailVm("CAST_EVAL: unknown target type tag");
	}
}

std::string RowSignatureCanon(const Database::Item &Row) {
	std::vector<std::pair<std::string, std::string>> Pairs;
	Pairs.reserve(Row.size());
	for(const auto &[K, V] : Row)
		Pairs.emplace_back(K, V);
	std::sort(Pairs.begin(), Pairs.end());
	std::ostringstream O;
	for(const auto &[K, V] : Pairs)
		O << '\0' << K << '\x01' << V;
	return std::move(O).str();
}

static Database::Item MapRowByColumnList(const Database::Item &Row, const std::vector<std::string> &SrcKeys,
                                         const std::vector<std::string> &DstKeys) {
	if(SrcKeys.size() != DstKeys.size())
		FailVm("SET_COMBINE internal projection mismatch.");
	Database::Item O;
	for(size_t I = 0; I < DstKeys.size(); ++I) {
		auto It = Row.find(SrcKeys[I]);
		O[DstKeys[I]] = It == Row.end() ? "" : It->second;
	}
	return O;
}

static std::string GroupKeySignature(const Database::Item &Row, const std::vector<std::string> &Keys) {
	Database::Item Sub;
	for(const auto &K : Keys) {
		auto It = Row.find(K);
		Sub[K] = It == Row.end() ? "" : It->second;
	}
	return RowSignatureCanon(Sub);
}

static void PadOlapOutputRows(Database::Table &Tbl, const std::vector<std::string> &AllKeys,
                              const std::vector<std::string> &ActiveKeys, int64_t OlapLevel) {
	for(auto &Row : Tbl) {
		for(const auto &K : AllKeys) {
			const bool Active =
			    std::find(ActiveKeys.begin(), ActiveKeys.end(), K) != ActiveKeys.end();
			if(!Active)
				Row[K] = "";
			Row["_grouping_" + K] = Active ? "0" : "1";
		}
		Row["_olap_level"] = std::to_string(OlapLevel);
	}
}

static size_t GroupByInstPayloadEnd(const Instruction &Inst);

/** Single grouping pass; \a ActiveKeys may be a prefix of the keys encoded in \a Inst operands. */
static void RunGroupByCore(Database::Table &Tbl, const Instruction &Inst,
                           const std::vector<std::string> &ActiveKeys) {
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk)
		FailVm("GROUP_BY bad operands");
	const int64_t AggMode = *Tag;
	if(AggMode == 3) {
		const size_t Base = static_cast<size_t>(2 + *Nk);
		if(Inst.Operands.size() < Base + 2)
			FailVm("GROUP_BY multi-aggregate: truncated header");
		const auto *HCnt = std::get_if<int64_t>(&Inst.Operands[Base]);
		const auto *Na = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
		if(!HCnt || !Na || *Na < 0 || *Na > 32 ||
		   Inst.Operands.size() < Base + 2 + static_cast<size_t>(*Na) * 3)
			FailVm("GROUP_BY multi-aggregate bad counts");
		const bool IncludeCountStar = (*HCnt != 0);
		const size_t IdxAfterSpecs = Base + 2 + static_cast<size_t>(*Na) * 3;
		const size_t PayloadEnd = GroupByInstPayloadEnd(Inst);
		if(PayloadEnd < IdxAfterSpecs)
			FailVm("GROUP_BY multi-aggregate bad counts");
		std::string CountStarCol = "cnt";
		if(IncludeCountStar) {
			if(PayloadEnd == IdxAfterSpecs + 1) {
				const auto *Cn = std::get_if<std::string>(&Inst.Operands[IdxAfterSpecs]);
				if(!Cn || Cn->empty())
					FailVm("GROUP_BY multi-aggregate: COUNT(*) output column name must be non-empty");
				CountStarCol = *Cn;
			} else if(PayloadEnd != IdxAfterSpecs)
				FailVm("GROUP_BY multi-aggregate bad counts");
		} else if(PayloadEnd != IdxAfterSpecs)
			FailVm("GROUP_BY multi-aggregate: unexpected trailing operands");
		struct AggSpecVm {
			int Kind = 0;
			std::string SrcCol;
			std::string OutCol;
		};
		std::vector<AggSpecVm> Specs;
		Specs.reserve(static_cast<size_t>(*Na));
		size_t Idx = Base + 2;
		for(int64_t A = 0; A < *Na; ++A) {
			const auto *Knd = std::get_if<int64_t>(&Inst.Operands[Idx++]);
			const auto *Sc = std::get_if<std::string>(&Inst.Operands[Idx++]);
			const auto *Ou = std::get_if<std::string>(&Inst.Operands[Idx++]);
			if(!Knd || !Sc || !Ou || Sc->empty() || Ou->empty())
				FailVm("GROUP_BY multi-aggregate bad spec");
			Specs.push_back(AggSpecVm{static_cast<int>(*Knd), *Sc, *Ou});
		}
		struct Accum {
			std::unordered_map<std::string, Database::Item> KeyTemplate;
			std::unordered_map<std::string, int64_t> CntStar;
			std::unordered_map<std::string, std::vector<double>> Sum;
			std::unordered_map<std::string, std::vector<int64_t>> AvgN;
			std::unordered_map<std::string, std::vector<bool>> HaveMinMax;
			std::unordered_map<std::string, std::vector<std::string>> CurMin;
			std::unordered_map<std::string, std::vector<std::string>> CurMax;
		} Acc;
		for(const auto &Row : Tbl) {
			const std::string Sig = GroupKeySignature(Row, ActiveKeys);
			if(IncludeCountStar)
				Acc.CntStar[Sig]++;
			if(Acc.KeyTemplate.find(Sig) == Acc.KeyTemplate.end()) {
				Database::Item R;
				for(const auto &K : ActiveKeys) {
					auto It = Row.find(K);
					R[K] = It == Row.end() ? "" : It->second;
				}
				Acc.KeyTemplate.emplace(Sig, std::move(R));
			}
			if(Acc.Sum.find(Sig) == Acc.Sum.end()) {
				Acc.Sum[Sig] = std::vector<double>(Specs.size(), 0.0);
				Acc.AvgN[Sig] = std::vector<int64_t>(Specs.size(), 0);
				Acc.HaveMinMax[Sig] = std::vector<bool>(Specs.size(), false);
				Acc.CurMin[Sig] = std::vector<std::string>(Specs.size());
				Acc.CurMax[Sig] = std::vector<std::string>(Specs.size());
			}
			auto &Sv = Acc.Sum[Sig];
			auto &Nv = Acc.AvgN[Sig];
			auto &Hm = Acc.HaveMinMax[Sig];
			auto &Cmin = Acc.CurMin[Sig];
			auto &Cmax = Acc.CurMax[Sig];
			for(size_t Si = 0; Si < Specs.size(); ++Si) {
				const auto &Sp = Specs[Si];
				auto It = Row.find(Sp.SrcCol);
				const std::string Cell = It == Row.end() ? "" : It->second;
				switch(Sp.Kind) {
					case static_cast<int>(GroupCombAggKind::Sum):
					case static_cast<int>(GroupCombAggKind::Avg): {
						double X = 0;
						bool Ok = false;
						try {
							X = std::stod(Cell);
							Ok = true;
						} catch(...) {
						}
						if(Ok) {
							Sv[Si] += X;
							if(Sp.Kind == static_cast<int>(GroupCombAggKind::Avg))
								Nv[Si]++;
						}
					} break;
					case static_cast<int>(GroupCombAggKind::Min): {
						if(!Hm[Si]) {
							Hm[Si] = true;
							Cmin[Si] = Cell;
						} else if(CompareScalars(Cell, Cmin[Si]) < 0)
							Cmin[Si] = Cell;
					} break;
					case static_cast<int>(GroupCombAggKind::Max): {
						if(!Hm[Si]) {
							Hm[Si] = true;
							Cmax[Si] = Cell;
						} else if(CompareScalars(Cell, Cmax[Si]) > 0)
							Cmax[Si] = Cell;
					} break;
					default:
						break;
				}
			}
		}
		Database::Table OutTbl;
		OutTbl.reserve(Acc.KeyTemplate.size());
		for(auto &Ky : Acc.KeyTemplate) {
			const std::string &Sig = Ky.first;
			Database::Item R = Ky.second;
			if(IncludeCountStar) {
				const auto ItCnt = Acc.CntStar.find(Sig);
				R[CountStarCol] = ItCnt == Acc.CntStar.end() ? "0" : std::to_string(ItCnt->second);
			}
			const auto &Sv = Acc.Sum.at(Sig);
			const auto &Nv = Acc.AvgN.at(Sig);
			const auto &Hm = Acc.HaveMinMax.at(Sig);
			const auto &Mn = Acc.CurMin.at(Sig);
			const auto &Mx = Acc.CurMax.at(Sig);
			for(size_t Si = 0; Si < Specs.size(); ++Si) {
				const auto &Sp = Specs[Si];
				switch(Sp.Kind) {
					case static_cast<int>(GroupCombAggKind::Sum):
						R[Sp.OutCol] = std::to_string(static_cast<long long>(std::llround(Sv[Si])));
						break;
					case static_cast<int>(GroupCombAggKind::Avg): {
						if(Nv[Si] > 0) {
							std::ostringstream O;
							O << (Sv[Si] / static_cast<double>(Nv[Si]));
							R[Sp.OutCol] = O.str();
						} else
							R[Sp.OutCol] = "0";
					} break;
					case static_cast<int>(GroupCombAggKind::Min):
						R[Sp.OutCol] = Hm[Si] ? Mn[Si] : "";
						break;
					case static_cast<int>(GroupCombAggKind::Max):
						R[Sp.OutCol] = Hm[Si] ? Mx[Si] : "";
						break;
					default:
						R[Sp.OutCol] = "";
						break;
				}
			}
			OutTbl.push_back(std::move(R));
		}
		Tbl = std::move(OutTbl);
		std::sort(Tbl.begin(), Tbl.end(), [&](const Database::Item &A, const Database::Item &B) {
			return GroupKeySignature(A, ActiveKeys) < GroupKeySignature(B, ActiveKeys);
		});
	} else if(AggMode == 1) {
		const size_t Base = static_cast<size_t>(2 + *Nk);
		const size_t PayloadEnd = GroupByInstPayloadEnd(Inst);
		std::string CountStarCol = "cnt";
		if(PayloadEnd == Base + 1) {
			const auto *Cn = std::get_if<std::string>(&Inst.Operands[Base]);
			if(!Cn || Cn->empty())
				FailVm("GROUP_BY COUNT(*): bad output column name operand");
			CountStarCol = *Cn;
		} else if(PayloadEnd != Base)
			FailVm("GROUP_BY COUNT(*): operand count mismatch");
		std::unordered_map<std::string, int64_t> Cnt;
		std::unordered_map<std::string, Database::Item> Template;
		Cnt.reserve(Tbl.size());
		Template.reserve(Tbl.size());
		for(const auto &Row : Tbl) {
			const std::string Sig = GroupKeySignature(Row, ActiveKeys);
			Cnt[Sig]++;
			if(Template.find(Sig) == Template.end()) {
				Database::Item R;
				for(const auto &K : ActiveKeys) {
					auto It = Row.find(K);
					R[K] = It == Row.end() ? "" : It->second;
				}
				R[CountStarCol] = "0";
				Template.emplace(Sig, std::move(R));
			}
		}
		Database::Table Out;
		Out.reserve(Cnt.size());
		for(auto &P : Cnt) {
			Database::Item RowOut = Template[P.first];
			RowOut[CountStarCol] = std::to_string(P.second);
			Out.push_back(std::move(RowOut));
		}
		Tbl = std::move(Out);
		std::sort(Tbl.begin(), Tbl.end(), [&](const Database::Item &A, const Database::Item &B) {
			return GroupKeySignature(A, ActiveKeys) < GroupKeySignature(B, ActiveKeys);
		});
	} else if(AggMode == 0) {
		std::unordered_map<std::string, Database::Item> First;
		First.reserve(Tbl.size());
		for(const auto &Row : Tbl) {
			const std::string Sig = GroupKeySignature(Row, ActiveKeys);
			if(First.find(Sig) == First.end())
				First.emplace(Sig, Row);
		}
		Database::Table Out;
		Out.reserve(First.size());
		for(auto &P : First)
			Out.push_back(std::move(P.second));
		Tbl = std::move(Out);
		std::sort(Tbl.begin(), Tbl.end(), [&](const Database::Item &A, const Database::Item &B) {
			return GroupKeySignature(A, ActiveKeys) < GroupKeySignature(B, ActiveKeys);
		});
	} else
		FailVm("GROUP_BY aggregate not supported for OLAP pass (use COUNT(*) or SUM/MIN/MAX/AVG)");
}

static void RunOlapModifier(Database::Table &Tbl, const Instruction &Inst,
                            const std::vector<std::string> &AllKeys, bool Cube) {
	const Database::Table Source = Tbl;
	Database::Table Combined;
	if(Cube) {
		const size_t N = AllKeys.size();
		if(N > 8)
			FailVm("CUBE supports at most 8 GROUP BY columns");
		const size_t Sets = size_t{1} << N;
		for(size_t Mask = 0; Mask < Sets; ++Mask) {
			std::vector<std::string> Active;
			Active.reserve(N);
			for(size_t I = 0; I < N; ++I) {
				if((Mask >> I) & 1)
					Active.push_back(AllKeys[I]);
			}
			Tbl = Source;
			RunGroupByCore(Tbl, Inst, Active);
			PadOlapOutputRows(Tbl, AllKeys, Active, static_cast<int64_t>(Mask));
			Combined.insert(Combined.end(), std::make_move_iterator(Tbl.begin()),
			                std::make_move_iterator(Tbl.end()));
		}
	} else {
		for(int Level = static_cast<int>(AllKeys.size()); Level >= 0; --Level) {
			std::vector<std::string> Active(AllKeys.begin(), AllKeys.begin() + Level);
			Tbl = Source;
			RunGroupByCore(Tbl, Inst, Active);
			PadOlapOutputRows(Tbl, AllKeys, Active, static_cast<int64_t>(Level));
			Combined.insert(Combined.end(), std::make_move_iterator(Tbl.begin()),
			                std::make_move_iterator(Tbl.end()));
		}
	}
	Tbl = std::move(Combined);
}

static size_t GroupByInstPayloadEnd(const Instruction &Inst) {
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk)
		return 0;
	const size_t Base = static_cast<size_t>(2 + *Nk);
	if(*Tag == 3) {
		if(Inst.Operands.size() < Base + 2)
			return 0;
		const auto *Na = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
		if(!Na)
			return 0;
		size_t End = Base + 2 + static_cast<size_t>(*Na) * 3;
		const auto *HCnt = std::get_if<int64_t>(&Inst.Operands[Base]);
		if(HCnt && *HCnt != 0 && Inst.Operands.size() > End) {
			const auto *Cn = std::get_if<std::string>(&Inst.Operands[End]);
			if(Cn && !Cn->empty())
				return End + 1;
		}
		return End;
	}
	if(*Tag == 4)
		return Inst.Operands.size() >= Base + 2 ? Base + 2 : 0;
	if(*Tag == 1) {
		const size_t WithCount = Base + 1;
		if(Inst.Operands.size() <= WithCount)
			return Inst.Operands.size();
		return WithCount;
	}
	if(*Tag == 0)
		return Base;
	return 0;
}

static void RunGroupingSetsOlap(Database::Table &Tbl, const Instruction &Inst,
                                const std::vector<std::string> &AllKeys) {
	const size_t PayloadEnd = GroupByInstPayloadEnd(Inst);
	if(PayloadEnd == 0 || PayloadEnd >= Inst.Operands.size())
		FailVm("GROUPING_SETS bad group-by payload");
	const auto *NSetsPtr = std::get_if<int64_t>(&Inst.Operands[PayloadEnd]);
	if(!NSetsPtr || *NSetsPtr < 0)
		FailVm("GROUPING SETS set count invalid");
	const size_t NSets = static_cast<size_t>(*NSetsPtr);
	size_t Idx = PayloadEnd + 1;
	std::vector<std::vector<std::string>> Sets;
	Sets.reserve(NSets);
	for(size_t S = 0; S < NSets; ++S) {
		auto *Nc = std::get_if<int64_t>(&Inst.Operands[Idx++]);
		if(!Nc || *Nc < 0)
			FailVm("GROUPING_SETS column count invalid");
		std::vector<std::string> One;
		One.reserve(static_cast<size_t>(*Nc));
		for(int64_t C = 0; C < *Nc; ++C) {
			auto *Col = std::get_if<std::string>(&Inst.Operands[Idx++]);
			if(!Col || Col->empty())
				FailVm("GROUPING_SETS column name expected");
			One.push_back(*Col);
		}
		Sets.push_back(std::move(One));
	}
	if(Idx != Inst.Operands.size())
		FailVm("GROUPING_SETS trailing operands");
	const Database::Table Source = Tbl;
	Database::Table Combined;
	for(size_t Si = 0; Si < Sets.size(); ++Si) {
		Tbl = Source;
		RunGroupByCore(Tbl, Inst, Sets[Si]);
		PadOlapOutputRows(Tbl, AllKeys, Sets[Si], static_cast<int64_t>(Si));
		Combined.insert(Combined.end(), std::make_move_iterator(Tbl.begin()),
		                std::make_move_iterator(Tbl.end()));
	}
	Tbl = std::move(Combined);
}

static Database::Item MergeJoinRowsPreferLeft(const Database::Item &LeftPrefer, const Database::Item &RightOther) {
	Database::Item J = LeftPrefer;
	for(const auto &[K, V] : RightOther) {
		auto It = J.find(K);
		if(It == J.end())
			J[K] = V;
		else if(It->second.empty() && !V.empty())
			It->second = V;
	}
	return J;
}

static bool JoinOnMatchPairs(const Database::Item &LeftRow, const Database::Item &RightRow,
                             const std::vector<std::pair<std::string, std::string>> &Pairs) {
	if(Pairs.empty())
		return true;
	for(const auto &[LCol, RCol] : Pairs) {
		auto Il = LeftRow.find(LCol);
		auto Ir = RightRow.find(RCol);
		const std::string Sl = Il == LeftRow.end() ? "" : Il->second;
		const std::string Sr = Ir == RightRow.end() ? "" : Ir->second;
		if(Sl != Sr)
			return false;
	}
	return true;
}

/** Caller must hold \c DbMutex_ exclusively (\c JoinTables VM path uses \c TableSchemaAssumeDbMutexHeld). */
static std::vector<std::string> CollectBlankColumnKeysFromSchema(const std::optional<Database::Schema> &Sch,
                                                                const Database::Table &Rows) {
	std::set<std::string> Names;
	if(Sch)
		for(const auto &Col : *Sch)
			Names.insert(Col.Name);
	for(const auto &Row : Rows) {
		for(const auto &[K, V] : Row)
			(void)V, Names.insert(K);
	}
	return std::vector<std::string>(Names.begin(), Names.end());
}

bool IsBytecodeConstraintTok(const std::string &Tok) {
	static const std::unordered_set<std::string> Constraints = {
	    "PRIMARY", "KEY", "NOT", "NULL", "UNIQUE", "AUTO_INCREMENT"};
	if(Constraints.count(Tok))
		return true;
	static const std::vector<std::string> Prefix = {"DEFAULT:", "CHECK:", "CHECKDNF:", "REFERENCES:",
	    "GENERATED:", "STUB_"};
	for(const auto &P : Prefix)
		if(Tok.size() >= P.size() && Tok.compare(0, P.size(), P) == 0)
			return true;
	return false;
}

void ApplyBytecodeConstraintTokens(Database::Column &Col, const std::vector<std::string> &Cons) {
	for(size_t i = 0; i < Cons.size(); ++i) {
		const std::string &T = Cons[i];
		if(T.rfind("DEFAULT:", 0) == 0) {
			const std::string Lit = T.size() > 8 ? T.substr(8) : "";
			if(Lit.empty() || Lit == "__NULL__")
				Col.InsertDefaultLiteral.reset();
			else
				Col.InsertDefaultLiteral = Lit;
		} else if(T.rfind("CHECKDNF:", 0) == 0)
			Col.CheckConstraintDnfPacked = T.size() > 9 ? T.substr(9) : "";
		else if(T.rfind("CHECK:", 0) == 0)
			Col.CheckConstraintSql = T.size() > 6 ? T.substr(6) : "";
		else if(T.rfind("REFERENCES:", 0) == 0) {
			const std::string R = T.substr(11);
			const size_t Pipe = R.find(':');
			if(Pipe != std::string::npos)
				Col.DeclaredFk = ForeignKey{Col.Name, R.substr(0, Pipe), R.substr(Pipe + 1)};
		} else if(T.rfind("STUB_", 0) == 0) {
			// Reserved prefixed placeholder constraint (no-op).
			(void)T;
		} else if(T == "PRIMARY" && i + 1 < Cons.size() && Cons[i + 1] == "KEY") {
			Col.IsPrimaryKey = true;
			++i;
		} else if(T == "UNIQUE")
			Col.IsUnique = true;
		else if(T == "NOT" && i + 1 < Cons.size() && Cons[i + 1] == "NULL") {
			Col.IsNotNull = true;
			++i;
		}
	}
}

static std::optional<std::string> ResolveStringImmediate(const Instruction &Inst,
                                                         const std::vector<std::string> *Pool) {
	if(Inst.Operands.empty())
		return std::nullopt;
	if(Inst.Opcode == Opcode::PUSH) {
		if(const auto *S = std::get_if<std::string>(&Inst.Operands[0]))
			return *S;
	}
	if(Inst.Opcode == Opcode::PUSH_POOL) {
		if(!Pool)
			FailVm("PUSH_POOL operand without string pool");
		if(const auto *Ix = std::get_if<int64_t>(&Inst.Operands[0])) {
			const size_t J = static_cast<size_t>(*Ix);
			if(J >= Pool->size())
				FailVm("string pool index overflow");
			return (*Pool)[J];
		}
	}
	return std::nullopt;
}

std::optional<std::string> StrPushOperand(const Bytecode &Code, size_t Idx,
                                        const std::vector<std::string> *Pool) {
	if(Idx >= Code.size())
		return std::nullopt;
	return ResolveStringImmediate(Code[Idx], Pool);
}

	std::optional<int64_t> IntPushOperand(const Bytecode &Code, size_t Idx) {
	if(Idx >= Code.size() || Code[Idx].Opcode != Opcode::PUSH)
		return std::nullopt;
	if(Code[Idx].Operands.empty())
		return std::nullopt;
	if(auto *V = std::get_if<int64_t>(&Code[Idx].Operands[0]))
		return *V;
	return std::nullopt;
}

std::string SanitizeSavepointSlug(const std::string &Raw) {
	std::string Out;
	for(unsigned char UC : Raw) {
		const char Ch = static_cast<char>(UC);
		if(std::isalnum(UC) || Ch == '_' || Ch == '-' || Ch == '.')
			Out.push_back(Ch);
	}
	return Out.empty() ? std::string("sp") : Out;
}

static constexpr std::streamoff kMaxVmDbFileCopyBytes = 512LL * 1024 * 1024;

/** Read-write whole file (\c trunc) so restores are not visibly partial mid-copy on hosted FS. */
static void VmCopyWholeFileOverwrite(const std::filesystem::path &Src, const std::filesystem::path &Dst) {
	std::ifstream In(Src, std::ios::binary | std::ios::ate);
	if(!In)
		FailVm("Cannot open source file \"" + Src.string() + "\" for whole-file restore.");
	std::streamoff Sz = In.tellg();
	if(Sz <= 0)
		FailVm("Source file \"" + Src.string() + "\" is empty or unreadable.");
	if(Sz > kMaxVmDbFileCopyBytes)
		FailVm("Source database file \"" + Src.string() + "\" is unexpectedly large.");
	if(static_cast<unsigned long long>(Sz) >
	   static_cast<unsigned long long>(std::numeric_limits<std::streamsize>::max()))
		FailVm("Source file size overflow.");
	In.seekg(0);
	std::vector<char> Buf(static_cast<size_t>(Sz));
	In.read(Buf.data(), Sz);
	if(!In || In.gcount() != Sz)
		FailVm("Short read restoring database from \"" + Src.string() + "\".");
	std::ofstream Out(Dst, std::ios::binary | std::ios::trunc);
	if(!Out)
		FailVm("Cannot open destination \"" + Dst.string() + "\" for database restore overwrite.");
	Out.write(Buf.data(), static_cast<std::streamsize>(Buf.size()));
	Out.flush();
	if(!Out)
		FailVm("Incomplete write restoring database \"" + Dst.string() + "\".");
}

void RemoveWalAdjacent(const std::filesystem::path &DbFile) {
	std::error_code Ec;
	std::filesystem::remove(std::filesystem::path(DbFile.string() + ".wal"), Ec);
}

} // namespace

bool EvaluatePackedWhereDnf(const Database *Db, const Database::Item &Row, std::string_view PackedDnfBlob) {
	return EvalPackedWhereDnfLocal(Db, Row, PackedDnfBlob);
}

void BytecodeInterpreter::CleanupStack() {
	while(!StackSlots_.empty())
		PopDiscardTopSlot();
}

void BytecodeInterpreter::PushScalarWord(uint64_t v) {
	if(StackSlots_.size() >= Limits::MaxInterpreterStackDepth)
		throw std::runtime_error("Bytecode VM stack depth limit exceeded");
	StackSlots_.push_back(VmStackSlot{v, false});
	Sp = StackSlots_.size();
}

void BytecodeInterpreter::PushOwningStringHeap(std::string *p) {
	if(StackSlots_.size() >= Limits::MaxInterpreterStackDepth)
		throw std::runtime_error("Bytecode VM stack depth limit exceeded");
	StackSlots_.push_back(
	    VmStackSlot{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)), true});
	Sp = StackSlots_.size();
}

uint64_t BytecodeInterpreter::PopScalarWord(const char *ctx) {
	if(StackSlots_.empty())
		FailVm(std::string("stack underflow (") + ctx + ")");
	VmStackSlot S = StackSlots_.back();
	StackSlots_.pop_back();
	Sp = StackSlots_.size();
	if(S.OwnsCppStringHeap) {
		delete reinterpret_cast<std::string *>(static_cast<uintptr_t>(S.Word));
		FailVm(std::string("expected numeric stack slot (") + ctx + ")");
	}
	return S.Word;
}

std::string BytecodeInterpreter::PopOwnedStringMoved(const char *ctx) {
	if(StackSlots_.empty())
		FailVm(std::string("stack underflow (") + ctx + ")");
	VmStackSlot S = StackSlots_.back();
	StackSlots_.pop_back();
	Sp = StackSlots_.size();
	if(!S.OwnsCppStringHeap)
		FailVm(std::string("expected string stack slot (") + ctx + ")");
	auto *Ps = reinterpret_cast<std::string *>(static_cast<uintptr_t>(S.Word));
	std::string Out = std::move(*Ps);
	delete Ps;
	return Out;
}

void BytecodeInterpreter::PopDiscardTopSlot() {
	if(StackSlots_.empty())
		return;
	VmStackSlot S = StackSlots_.back();
	StackSlots_.pop_back();
	Sp = StackSlots_.size();
	if(S.OwnsCppStringHeap)
		delete reinterpret_cast<std::string *>(static_cast<uintptr_t>(S.Word));
}

void BytecodeInterpreter::EnsurePrimaryDatabaseOpened() {
	if(Databases_.empty())
		Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
}

void BytecodeInterpreter::ReloadPrimaryDatabaseFromDisk() {
	if(Databases_.empty())
		return;
	const std::filesystem::path P = Databases_[0]->DbPath_;
	Logger *L = Databases_[0]->GetLogger();
	Databases_[0]->SetSkipExitSyncOnDestroy(true);
	Databases_.clear();
	Databases_.push_back(std::make_unique<Database>(P, L));
}

void BytecodeInterpreter::Execute(const Bytecode &Code) {
	Execute(Code, nullptr);
}

void BytecodeInterpreter::Execute(const CompiledBytecode &Compiled) {
	Execute(Compiled.Instructions,
	        Compiled.StringPool.empty() ? nullptr : &Compiled.StringPool);
}

void BytecodeInterpreter::Execute(const Bytecode &Code, const std::vector<std::string> *StringPool) {
	Reset();
	StringOperandPool_ = StringPool;
	StepsExecuted_ = 0;
	while(Ic < Code.size()) {
		if(++StepsExecuted_ > Limits::MaxInterpreterSteps)
			FailVm("Statement exceeded the VM step limit (safety guard against infinite loops or oversized programs).");
		if(!Step(Code))
			break;
	}
	StringOperandPool_ = nullptr;
}

bool BytecodeInterpreter::Step(const Bytecode &Code) {
    if (Ic >= Code.size()) return false;
    const Instruction &inst = Code[Ic];
    switch (inst.Opcode) {
        case Opcode::NOP:
            ++Ic;
            break;
        case Opcode::HALT:
            return false;
        case Opcode::PUSH: {
            if (inst.Operands.empty()) FailVm("PUSH requires operand");
            if (auto Val = std::get_if<int64_t>(&inst.Operands[0])) {
                PushScalarWord(static_cast<uint64_t>(*Val));
            } else if (auto Str = std::get_if<std::string>(&inst.Operands[0])) {
                PushOwningStringHeap(AllocateVmImmediateString(*Str));
            } else {
                FailVm("PUSH only supports int64_t or string operand");
            }
            ++Ic;
            break;
        }
        case Opcode::PUSH_POOL: {
            if (inst.Operands.empty())
                FailVm("PUSH_POOL requires pool index operand");
            if (!StringOperandPool_)
                FailVm("PUSH_POOL used without bytecode string pool");
            if (auto Ix = std::get_if<int64_t>(&inst.Operands[0])) {
                const size_t Idx = static_cast<size_t>(*Ix);
                if (Idx >= StringOperandPool_->size())
                    FailVm("PUSH_POOL index out of range");
                PushOwningStringHeap(
                    AllocateVmImmediateString(StringOperandPool_->at(Idx)));
            } else {
                FailVm("PUSH_POOL expects int64 operand");
            }
            ++Ic;
            break;
        }
        case Opcode::POP: {
            if (StackSlots_.empty()) {
                ++Ic;
                break;
            }
            PopDiscardTopSlot();
            ++Ic;
            break;
        }
        // Arithmetic example: ADD
        case Opcode::ADD: {
            uint64_t b = PopScalarWord("VM ADD");
            uint64_t a = PopScalarWord("VM ADD");
            PushScalarWord(a + b);
            ++Ic;
            break;
        }
        // Control flow: JMP
        case Opcode::JMP: {
            if (inst.Operands.empty()) FailVm("JMP requires target operand");
            if (auto target = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*target < 0 || static_cast<size_t>(*target) >= Code.size())
                    FailVm("JMP target out of range");
                Ic = static_cast<uintptr_t>(*target);
            } else {
                FailVm("JMP expects int64_t operand");
            }
            break;
        }
        // Control flow: CALL (push return address, jump)
        case Opcode::CALL: {
            if (inst.Operands.empty()) FailVm("CALL requires target operand");
            if (auto target = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*target < 0 || static_cast<size_t>(*target) >= Code.size())
                    FailVm("CALL target out of range");
                PushScalarWord(Ic + 1); // Push return address
                Ic = static_cast<uintptr_t>(*target);
            } else {
                FailVm("CALL expects int64_t operand");
            }
            break;
        }
        // Control flow: RET (pop return address)
        case Opcode::RET: {
            if (StackSlots_.empty()) FailVm("RET with empty stack");
            Ic = static_cast<uintptr_t>(PopScalarWord("RET"));
            break;
        }
        // Arithmetic: SUB
        case Opcode::SUB: {
            uint64_t b = PopScalarWord("VM SUB");
            uint64_t a = PopScalarWord("VM SUB");
            PushScalarWord(a - b);
            ++Ic;
            break;
        }
        // Arithmetic: MUL
        case Opcode::MUL: {
            uint64_t b = PopScalarWord("VM MUL");
            uint64_t a = PopScalarWord("VM MUL");
            PushScalarWord(a * b);
            ++Ic;
            break;
        }
        // Arithmetic: DIV
        case Opcode::DIV: {
            uint64_t b = PopScalarWord("VM DIV");
            uint64_t a = PopScalarWord("VM DIV");
            if (b == 0) FailVm("Division by zero is not allowed.");
            PushScalarWord(a / b);
            ++Ic;
            break;
        }
        // Arithmetic: MOD
        case Opcode::MOD: {
            uint64_t b = PopScalarWord("VM MOD");
            uint64_t a = PopScalarWord("VM MOD");
            if (b == 0) FailVm("Modulo by zero is not allowed.");
            PushScalarWord(a % b);
            ++Ic;
            break;
        }
        // Database operations
        case Opcode::CREATE_TABLE: {
            if (inst.Operands.empty()) FailVm("CREATE_TABLE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                int64_t IfNotExists = 0;
                if (inst.Operands.size() > 2) {
                    if (auto Fl = std::get_if<int64_t>(&inst.Operands[2]))
                        IfNotExists = *Fl;
                }
                int64_t TableConstraintBlocks = 0;
                if (inst.Operands.size() > 3) {
                    if (auto Nb = std::get_if<int64_t>(&inst.Operands[3]))
                        TableConstraintBlocks = *Nb;
                }
                if (TableConstraintBlocks < 0 || TableConstraintBlocks > 256)
                    FailVm("CREATE_TABLE invalid table constraint count");
                bool SkipCreate = false;
                Databases_[0]->WithExclusiveBytecodeLock([&]() {
                    SkipCreate = IfNotExists != 0 && Databases_[0]->Tables_.count(*tableName) != 0;
                });

                size_t J = Ic + 1;
                Database::Schema Schema;
                if (inst.Operands.size() >= 2) {
                    auto *nColPtr = std::get_if<int64_t>(&inst.Operands[1]);
                    if (!nColPtr || *nColPtr < 0 || *nColPtr > 10000)
                        FailVm("CREATE_TABLE invalid column count");
                    const size_t NCols = static_cast<size_t>(*nColPtr);
                    for (size_t C = 0; C < NCols; ++C) {
                        auto ColName = StrPushOperand(Code, J, StringOperandPool_);
                        if (!ColName)
                            FailVm("CREATE_TABLE missing column name");
                        ++J;
                        auto ColType = StrPushOperand(Code, J, StringOperandPool_);
                        if (!ColType)
                            FailVm("CREATE_TABLE missing column type");
                        ++J;
                        auto NCons = IntPushOperand(Code, J);
                        if (!NCons || *NCons < 0 || *NCons > 128)
                            FailVm("CREATE_TABLE invalid constraint count");
                        ++J;
                        std::vector<std::string> Cons;
                        for (int64_t KK = 0; KK < *NCons; ++KK) {
                            auto T = StrPushOperand(Code, J, StringOperandPool_);
                            if (!T)
                                FailVm("CREATE_TABLE missing constraint token");
                            Cons.push_back(*T);
                            ++J;
                        }
                        if (!SkipCreate) {
                            Database::Column Col;
                            Col.Name = *ColName;
                            Col.DefaultValue = *ColType;
                            Col.IsPrimaryKey = false;
                            Col.IsUnique = false;
                            Col.IsNotNull = false;
                            ApplyBytecodeConstraintTokens(Col, Cons);
                            Schema.push_back(std::move(Col));
                        }
                    }
                } else {
                    while (
                        J < Code.size()
                        && (Code[J].Opcode == Opcode::PUSH || Code[J].Opcode == Opcode::PUSH_POOL)) {
                        auto ColName = StrPushOperand(Code, J, StringOperandPool_);
                        if (!ColName || IsBytecodeConstraintTok(*ColName))
                            break;
                        ++J;
                        auto ColType = StrPushOperand(Code, J, StringOperandPool_);
                        if (!ColType) {
                            --J;
                            break;
                        }
                        ++J;
                        std::vector<std::string> Cons;
                        while (J < Code.size()
                               && (Code[J].Opcode == Opcode::PUSH || Code[J].Opcode == Opcode::PUSH_POOL)) {
                            auto T = StrPushOperand(Code, J, StringOperandPool_);
                            if (!T || !IsBytecodeConstraintTok(*T))
                                break;
                            Cons.push_back(*T);
                            ++J;
                        }
                        if (!SkipCreate) {
                            Database::Column Col;
                            Col.Name = *ColName;
                            Col.DefaultValue = *ColType;
                            Col.IsPrimaryKey = false;
                            Col.IsUnique = false;
                            Col.IsNotNull = false;
                            ApplyBytecodeConstraintTokens(Col, Cons);
                            Schema.push_back(std::move(Col));
                        }
                    }
                }

                auto ApplyTcToSchema = [&Schema](const std::string &Kind, const std::vector<std::string> &Names) {
                    for (const std::string &Nm : Names) {
                        for (auto &SchCol : Schema) {
                            if (SchCol.Name == Nm) {
                                if (Kind == "__PK__")
                                    SchCol.IsPrimaryKey = true;
                                else if (Kind == "__UQ__")
                                    SchCol.IsUnique = true;
                                break;
                            }
                        }
                    }
                };

                std::vector<ForeignKey> PostCreateFks;
                std::vector<std::pair<std::string, std::string>> PendingTableChecks;
                for (int64_t Tb = 0; Tb < TableConstraintBlocks; ++Tb) {
                    auto Kstr = StrPushOperand(Code, J, StringOperandPool_);
                    if (!Kstr)
                        FailVm("CREATE_TABLE missing table constraint kind");
                    ++J;
                    if (*Kstr == "__PK__" || *Kstr == "__UQ__") {
                        auto NN = IntPushOperand(Code, J);
                        if (!NN || *NN < 1 || *NN > 128)
                            FailVm("CREATE_TABLE bad PK/UQ arity");
                        ++J;
                        std::vector<std::string> Names;
                        for (int64_t k = 0; k < *NN; ++k) {
                            auto P = StrPushOperand(Code, J, StringOperandPool_);
                            if (!P)
                                FailVm("CREATE_TABLE PK/UQ column name");
                            Names.push_back(*P);
                            ++J;
                        }
                        if (!SkipCreate)
                            ApplyTcToSchema(*Kstr, Names);
                    } else if (*Kstr == "__FK__") {
                        auto NN = IntPushOperand(Code, J);
                        if (!NN || *NN != 1)
                            FailVm("CREATE_TABLE composite FK not supported yet");
                        ++J;
                        auto Lcol = StrPushOperand(Code, J, StringOperandPool_);
                        if (!Lcol)
                            FailVm("CREATE_TABLE FK local column");
                        ++J;
                        auto Rt = StrPushOperand(Code, J, StringOperandPool_);
                        if (!Rt)
                            FailVm("CREATE_TABLE FK ref table");
                        ++J;
                        auto Rc = StrPushOperand(Code, J, StringOperandPool_);
                        if (!Rc)
                            FailVm("CREATE_TABLE FK ref column");
                        ++J;
                        if (!SkipCreate)
                            PostCreateFks.push_back(ForeignKey{*Lcol, *Rt, *Rc});
                    } else if (*Kstr == "__CK__") {
                        auto Sql = StrPushOperand(Code, J, StringOperandPool_);
                        if (!Sql)
                            FailVm("CREATE_TABLE CHECK SQL operand missing");
                        ++J;
                        auto Blob = StrPushOperand(Code, J, StringOperandPool_);
                        if (!Blob)
                            FailVm("CREATE_TABLE CHECK packed predicate operand missing");
                        ++J;
                        if (!SkipCreate)
                            PendingTableChecks.emplace_back(*Sql, std::move(*Blob));
                    } else
                        FailVm("CREATE_TABLE unknown table constraint tag");
                }

                if (!SkipCreate) {
                    Databases_[0]->CreateTable(*tableName, Schema).get();
                    for (const Database::Column &Co : Schema) {
                        if (Co.DeclaredFk.has_value())
                            Databases_[0]->AddForeignKey(*tableName, *Co.DeclaredFk).get();
                    }
                    for (const ForeignKey &Fk : PostCreateFks)
                        Databases_[0]->AddForeignKey(*tableName, Fk).get();
                    if (!PendingTableChecks.empty())
                        Databases_[0]->ReplaceTableLevelCheckConstraints(*tableName,
                                                                         std::move(PendingTableChecks));
                }
                Ic = J;
            } else {
                FailVm("CREATE_TABLE expects string operand");
            }
            break;
        }
        case Opcode::DROP_TABLE: {
            if (inst.Operands.empty()) FailVm("DROP_TABLE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                int64_t IfExists = 0;
                if (inst.Operands.size() > 1)
                    if (auto F = std::get_if<int64_t>(&inst.Operands[1]))
                        IfExists = *F;
                if (IfExists != 0) {
                    bool SkipDrop = false;
                    Databases_[0]->WithExclusiveBytecodeLock([&]() {
                        SkipDrop =
                            Databases_[0]->Tables_.find(*tableName) == Databases_[0]->Tables_.end();
                    });
                    if (SkipDrop) {
                        ++Ic;
                        break;
                    }
                }
                Databases_[0]->DropTable(*tableName).get();
            } else {
                FailVm("DROP_TABLE expects string operand");
            }
            ++Ic;
            break;
        }
        case Opcode::CREATE_VIEW: {
            if (inst.Operands.size() < 2)
                FailVm("CREATE_VIEW requires view name and body SQL operands");
            if (auto vn = std::get_if<std::string>(&inst.Operands[0])) {
                auto body = std::get_if<std::string>(&inst.Operands[1]);
                if(!body)
                    FailVm("CREATE_VIEW body operand must be a string");
                if (Databases_.empty())
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
                Databases_[0]->DefineView(*vn, *body);
            } else {
                FailVm("CREATE_VIEW expects string view name");
            }
            ++Ic;
            break;
        }
        case Opcode::DROP_VIEW: {
            if (inst.Operands.empty())
                FailVm("DROP_VIEW requires view name operand");
            if (auto vn = std::get_if<std::string>(&inst.Operands[0])) {
                int64_t IfExists = 0;
                if(inst.Operands.size() > 1)
                    if(auto F = std::get_if<int64_t>(&inst.Operands[1]))
                        IfExists = *F;
                if (Databases_.empty())
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
                Databases_[0]->DropViewDefinition(*vn, IfExists != 0);
            } else {
                FailVm("DROP_VIEW expects string view name");
            }
            ++Ic;
            break;
        }
        case Opcode::ALTER_TABLE: {
            if (inst.Operands.size() < 2)
                FailVm("ALTER_TABLE requires kind and operands");
            if (Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
            auto *AlterKind = std::get_if<int64_t>(&inst.Operands[0]);
            if (!AlterKind)
                FailVm("ALTER_TABLE kind must be int64");
            if (*AlterKind == 0) {
                if (inst.Operands.size() < 5)
                    FailVm("ALTER TABLE ADD COLUMN operands");
                auto *Ta = std::get_if<std::string>(&inst.Operands[1]);
                auto *ColN = std::get_if<std::string>(&inst.Operands[2]);
                auto *ColT = std::get_if<std::string>(&inst.Operands[3]);
                auto *NCons = std::get_if<int64_t>(&inst.Operands[4]);
                if (!Ta || !ColN || !ColT || !NCons || *NCons < 0
                    || inst.Operands.size() != static_cast<size_t>(*NCons + 5))
                    FailVm("ALTER TABLE ADD malformed");
                std::vector<std::string> Cons;
                for (int64_t i = 0; i < *NCons; ++i)
                    if (auto *S = std::get_if<std::string>(&inst.Operands[5 + static_cast<size_t>(i)]))
                        Cons.push_back(*S);
                    else
                        FailVm("ALTER TABLE ADD constraint expects string");
                Database::Column Col;
                Col.Name = *ColN;
                Col.DefaultValue = *ColT;
                ApplyBytecodeConstraintTokens(Col, Cons);
                Db->AddColumn(*Ta, Col).get();
            } else if (*AlterKind == 1) {
                if (inst.Operands.size() < 3)
                    FailVm("ALTER TABLE DROP COLUMN operands");
                auto *Ta = std::get_if<std::string>(&inst.Operands[1]);
                auto *Cn = std::get_if<std::string>(&inst.Operands[2]);
                if (!Ta || !Cn)
                    FailVm("ALTER TABLE DROP expects table and column strings");
                Db->DropColumn(*Ta, *Cn).get();
            } else if (*AlterKind == 2) {
                if(inst.Operands.size() < 4)
                    FailVm("ALTER TABLE RENAME COLUMN operands");
                auto *RnFrom = std::get_if<std::string>(&inst.Operands[2]);
                auto *RnTo = std::get_if<std::string>(&inst.Operands[3]);
                if(!RnFrom || !RnTo)
                    FailVm("ALTER TABLE RENAME COLUMN names");
                if(Logger_)
                    Logger_->Info("ALTER TABLE RENAME COLUMN stub (not applied): \""
                                   + *RnFrom + "\" -> \"" + *RnTo + '\"');
                (void)Db;
            } else
                FailVm("ALTER_TABLE unknown alteration kind");
            ++Ic;
            break;
        }
        case Opcode::INSERT: {
            if (inst.Operands.size() < 2) FailVm("INSERT requires value count and column mode operands");
            auto *KPtr = std::get_if<int64_t>(&inst.Operands[0]);
            auto *ExplicitPtr = std::get_if<int64_t>(&inst.Operands[1]);
            if (!KPtr || !ExplicitPtr) FailVm("INSERT expects int64 operands");
            const int64_t K64 = *KPtr;
            if (K64 < 0 || K64 > 100000) FailVm("INSERT value count out of range");
            const size_t K = static_cast<size_t>(K64);
            if (Databases_.empty()) {
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            }
            Database *Db = Databases_[0].get();

            auto PopBorrowedStr = [&]() -> std::string {
                return PopOwnedStringMoved("INSERT");
            };

            std::vector<std::string> Values(K);
            for (size_t i = K; i-- > 0;)
                Values[i] = PopBorrowedStr();

            Database::Item Row;
            if (*ExplicitPtr) {
                std::vector<std::string> ColKeys(K);
                for (size_t i = K; i-- > 0;)
                    ColKeys[i] = PopBorrowedStr();
                const std::string TableName = PopBorrowedStr();
                for (size_t i = 0; i < K; ++i)
                    Row[ColKeys[i]] = Values[i];
                Db->Insert(TableName, Row).get();
            } else {
                const std::string TableName = PopBorrowedStr();
                auto Snap = Db->TableSchemaSnapshot(TableName);
                if (!Snap || Snap->size() != K)
                    FailVm("INSERT implicit columns require matching table schema");
                for (size_t i = 0; i < K; ++i)
                    Row[(*Snap)[i].Name] = Values[i];
                Db->Insert(TableName, Row).get();
            }
            ++Ic;
            break;
        }
        case Opcode::UPSERT: {
            if(inst.Operands.size() < 5)
                FailVm("UPSERT malformed operands");
            auto *KPtr = std::get_if<int64_t>(&inst.Operands[0]);
            auto *ExplicitPtr = std::get_if<int64_t>(&inst.Operands[1]);
            auto *DoNothingPtr = std::get_if<int64_t>(&inst.Operands[2]);
            auto *NConflictPtr = std::get_if<int64_t>(&inst.Operands[3]);
            if(!KPtr || !ExplicitPtr || !DoNothingPtr || !NConflictPtr)
                FailVm("UPSERT expects int64 header operands");
            if(*NConflictPtr < 0)
                FailVm("UPSERT conflict column count invalid");
            const size_t NConflict = static_cast<size_t>(*NConflictPtr);
            if(inst.Operands.size() < 4 + NConflict + 1)
                FailVm("UPSERT missing update assignment count");
            const size_t SetCountIdx = 4 + NConflict;
            auto *NSetPtr = std::get_if<int64_t>(&inst.Operands[SetCountIdx]);
            if(!NSetPtr || *NSetPtr < 0)
                FailVm("UPSERT SET count invalid");
            const size_t NSet = static_cast<size_t>(*NSetPtr);
            if(inst.Operands.size() != SetCountIdx + 1 + 2 * NSet)
                FailVm("UPSERT operand tail size mismatch");
            std::vector<std::string> ConflictCols;
            ConflictCols.reserve(NConflict);
            for(size_t i = 0; i < NConflict; ++i) {
                if(auto *S = std::get_if<std::string>(&inst.Operands[4 + i]))
                    ConflictCols.push_back(*S);
                else
                    FailVm("UPSERT conflict column name must be string");
            }
            std::vector<std::pair<std::string, std::string>> UpdateAssignments;
            const size_t PairsBase = SetCountIdx + 1;
            const int64_t K64 = *KPtr;
            if(K64 < 0 || K64 > 100000)
                FailVm("UPSERT value count out of range");
            const size_t K = static_cast<size_t>(K64);
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
            auto PopBorrowedStr = [&]() -> std::string { return PopOwnedStringMoved("UPSERT"); };
            std::vector<std::string> Values(K);
            for(size_t i = K; i-- > 0;)
                Values[i] = PopBorrowedStr();
            Database::Item Row;
            std::string TableName;
            if(*ExplicitPtr) {
                std::vector<std::string> ColKeys(K);
                for(size_t i = K; i-- > 0;)
                    ColKeys[i] = PopBorrowedStr();
                TableName = PopBorrowedStr();
                for(size_t i = 0; i < K; ++i)
                    Row[ColKeys[i]] = Values[i];
            } else {
                TableName = PopBorrowedStr();
                auto Snap = Db->TableSchemaSnapshot(TableName);
                if(!Snap || Snap->size() != K)
                    FailVm("UPSERT implicit columns require matching table schema");
                for(size_t i = 0; i < K; ++i)
                    Row[(*Snap)[i].Name] = Values[i];
            }
            if(*DoNothingPtr == 0 && NSet > 0) {
                UpdateAssignments.reserve(NSet);
                for(size_t s = 0; s < NSet; ++s) {
                    auto *ColN = std::get_if<std::string>(&inst.Operands[PairsBase + s * 2]);
                    auto *Blob = std::get_if<std::string>(&inst.Operands[PairsBase + s * 2 + 1]);
                    if(!ColN || !Blob)
                        FailVm("UPSERT DO UPDATE SET expects column/expression");
                    UpdateAssignments.emplace_back(*ColN, *Blob);
                }
            }
            Db->Upsert(TableName, Row, ConflictCols, *DoNothingPtr != 0, UpdateAssignments).get();
            ++Ic;
            break;
        }

        case Opcode::MERGE_INTO: {
            if(inst.Operands.size() < 4)
                FailVm("MERGE_INTO missing header operands");
            auto *TgtTbl = std::get_if<std::string>(&inst.Operands[0]);
            auto *SrcTbl = std::get_if<std::string>(&inst.Operands[1]);
            auto *NkPtr = std::get_if<int64_t>(&inst.Operands[2]);
            if(!TgtTbl || !SrcTbl || !NkPtr || *NkPtr < 0)
                FailVm("MERGE_INTO table/key operands");
            const size_t Nk = static_cast<size_t>(*NkPtr);
            size_t Idx = 3;
            if(inst.Operands.size() < Idx + Nk * 2 + 1)
                FailVm("MERGE_INTO truncated key pairs");
            std::vector<std::pair<std::string, std::string>> KeyPairs;
            KeyPairs.reserve(Nk);
            for(size_t i = 0; i < Nk; ++i) {
                auto *Tcol = std::get_if<std::string>(&inst.Operands[Idx]);
                auto *Scol = std::get_if<std::string>(&inst.Operands[Idx + 1]);
                if(!Tcol || !Scol)
                    FailVm("MERGE_INTO key pair expected");
                KeyPairs.emplace_back(*Tcol, *Scol);
                Idx += 2;
            }
            auto *NmPtr = std::get_if<int64_t>(&inst.Operands[Idx]);
            if(!NmPtr || *NmPtr < 0)
                FailVm("MERGE_INTO matched count");
            const size_t Nm = static_cast<size_t>(*NmPtr);
            ++Idx;
            if(inst.Operands.size() < Idx + Nm * 2 + 1)
                FailVm("MERGE_INTO truncated matched cells");
            std::vector<AstralDB::MergeUpdateCell> OnMatch;
            OnMatch.reserve(Nm);
            for(size_t i = 0; i < Nm; ++i) {
                auto *Tcol = std::get_if<std::string>(&inst.Operands[Idx]);
                auto *Blob = std::get_if<std::string>(&inst.Operands[Idx + 1]);
                if(!Tcol || !Blob)
                    FailVm("MERGE_INTO matched cell pair");
                AstralDB::MergeUpdateCell C;
                C.TargetColumn = *Tcol;
                C.ValueExpr = *Blob;
                OnMatch.push_back(std::move(C));
                Idx += 2;
            }
            auto *NiPtr = std::get_if<int64_t>(&inst.Operands[Idx]);
            if(!NiPtr || *NiPtr < 0)
                FailVm("MERGE_INTO insert count");
            const size_t Ni = static_cast<size_t>(*NiPtr);
            ++Idx;
            if(inst.Operands.size() != Idx + Ni * 2)
                FailVm("MERGE_INTO insert cells size mismatch");
            std::vector<AstralDB::MergeInsertCell> OnInsert;
            OnInsert.reserve(Ni);
            for(size_t i = 0; i < Ni; ++i) {
                auto *Icol = std::get_if<std::string>(&inst.Operands[Idx]);
                auto *Blob = std::get_if<std::string>(&inst.Operands[Idx + 1]);
                if(!Icol || !Blob)
                    FailVm("MERGE_INTO insert cell pair");
                AstralDB::MergeInsertCell C;
                C.Column = *Icol;
                C.ValueExpr = *Blob;
                OnInsert.push_back(std::move(C));
                Idx += 2;
            }
            if(Databases_.empty()) {
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            }
            Databases_[0]->MergeUsing(*TgtTbl, *SrcTbl, KeyPairs, OnMatch, OnInsert).get();
            ++Ic;
            break;
        }
        case Opcode::DELETE: {
            if (inst.Operands.empty()) FailVm("DELETE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                Databases_[0]->Delete(*tableName, [](const std::unordered_map<std::string, std::string>&) { return true; }).get();
            } else {
                FailVm("DELETE expects string operand");
            }
            ++Ic;
            break;
        }
        case Opcode::UPDATE: {
            if (inst.Operands.size() < 3) FailVm("UPDATE requires table name, column, and value operands");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (auto column = std::get_if<std::string>(&inst.Operands[1])) {
                    if (auto value = std::get_if<std::string>(&inst.Operands[2])) {
                        if (Databases_.empty()) {
                            Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                        }
                        std::unordered_map<std::string, std::string> newValues;
                        newValues[*column] = *value;
                        Databases_[0]->Update(*tableName, [](const std::unordered_map<std::string, std::string>&) { return true; }, newValues).get();
                    } else {
                        FailVm("UPDATE expects string value operand");
                    }
                } else {
                    FailVm("UPDATE expects string column operand");
                }
            } else {
                FailVm("UPDATE expects string table name operand");
            }
            ++Ic;
            break;
        }
        case Opcode::SELECT: {
            if (inst.Operands.empty()) FailVm("SELECT requires operand");
            if(const auto Count = std::get_if<int64_t>(&inst.Operands[0])) {
                if(*Count < 0 || *Count > 256)
                    FailVm("SELECT finalize: invalid column count");
                const size_t Need = static_cast<size_t>(*Count) + 1;
                if(StackSlots_.size() < Need)
                    FailVm("SELECT finalize: stack underflow");
                (void)PopOwnedStringMoved("SELECT finalize table");
                for(int64_t I = 0; I < *Count; ++I)
                    (void)PopOwnedStringMoved("SELECT finalize column");
				++Ic;
				break;
			}
            if (auto Column = std::get_if<std::string>(&inst.Operands[0])) {
                PushOwningStringHeap(new std::string(*Column));
            } else {
                FailVm("SELECT expects string column operand");
            }
            ++Ic;
            break;
        }
        case Opcode::SET: {
            if (inst.Operands.size() < 2) FailVm("SET requires column and value operands");
            if (auto Column = std::get_if<std::string>(&inst.Operands[0])) {
                if (auto Value = std::get_if<std::string>(&inst.Operands[1])) {
                    PushOwningStringHeap(new std::string(*Column));
                    PushOwningStringHeap(new std::string(*Value));
                } else {
                    FailVm("SET expects string value operand");
                }
            } else {
                FailVm("SET expects string column operand");
            }
            ++Ic;
            break;
        }
        case Opcode::WHERE: {
            Flags |= 0x1;
            ++Ic;
            break;
        }
        case Opcode::ORDER_BY: {
            if (inst.Operands.empty()) FailVm("ORDER_BY requires column name operand");
            if (auto Column = std::get_if<std::string>(&inst.Operands[0])) {
                if (StackSlots_.size() < 2)
                    FailVm("ORDER_BY requires ascending flag and table name on stack");
                const bool Ascending = PopScalarWord("ORDER_BY ascending") != 0;
                std::string TableName = PopOwnedStringMoved("ORDER_BY table");
                
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                Databases_[0]->WithExclusiveBytecodeLock([&]() {
                    auto &Table = Databases_[0]->Tables_[TableName];
                    std::sort(Table.begin(), Table.end(),
                        [Column, Ascending](const Database::Item& a, const Database::Item& b) {
                            auto itA = a.find(*Column);
                            auto itB = b.find(*Column);
                            if (itA == a.end() && itB == b.end()) return false;
                            if (itA == a.end()) return Ascending;
                            if (itB == b.end()) return !Ascending;
                            return Ascending ? (itA->second < itB->second) : (itA->second > itB->second);
                        });
                });
                PushOwningStringHeap(new std::string(TableName));
            } else {
                FailVm("ORDER_BY expects string column operand");
            }
            ++Ic;
            break;
        }
        case Opcode::GROUP_BY: {
            if(inst.Operands.size() < 2)
                FailVm("GROUP_BY expects mode, key count, and key columns");
            const auto *Tag = std::get_if<int64_t>(&inst.Operands[0]);
            const auto *Nk = std::get_if<int64_t>(&inst.Operands[1]);
            if(!Tag || !Nk || *Nk < 0 || *Nk > 64 || inst.Operands.size() < static_cast<size_t>(2 + *Nk))
                FailVm("GROUP_BY bad operand layout");
            std::vector<std::string> Keys;
            Keys.reserve(static_cast<size_t>(*Nk));
            for(int64_t I = 0; I < *Nk; ++I) {
                const auto *Ks = std::get_if<std::string>(&inst.Operands[static_cast<size_t>(2 + I)]);
                if(!Ks || Ks->empty())
                    FailVm("GROUP_BY key must be non-empty string");
                Keys.push_back(*Ks);
            }
            if(StackSlots_.empty())
                FailVm("GROUP_BY expects table name on stack");
            std::string TableName = PopOwnedStringMoved("GROUP_BY table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *DbGrp = Databases_[0].get();
            const int64_t AggMode = *Tag;
            DbGrp->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = DbGrp->Tables_[TableName];
            if(AggMode == 3) {
                const size_t Base = static_cast<size_t>(2 + *Nk);
                if(inst.Operands.size() < Base + 2)
                    FailVm("GROUP_BY multi-aggregate: truncated header");
                const auto *HCnt = std::get_if<int64_t>(&inst.Operands[Base]);
                const auto *Na = std::get_if<int64_t>(&inst.Operands[Base + 1]);
                if(!HCnt || !Na || *Na < 0 || *Na > 32 ||
                   inst.Operands.size() < Base + 2 + static_cast<size_t>(*Na) * 3)
                    FailVm("GROUP_BY multi-aggregate bad counts");
                const bool IncludeCountStar = (*HCnt != 0);
                const size_t IdxAfterSpecs = Base + 2 + static_cast<size_t>(*Na) * 3;
                std::string CountStarCol = "cnt";
                size_t SpecEnd = IdxAfterSpecs;
                if(IncludeCountStar) {
                    if(inst.Operands.size() == IdxAfterSpecs + 1) {
                        const auto *Cn = std::get_if<std::string>(&inst.Operands[IdxAfterSpecs]);
                        if(!Cn || Cn->empty())
                            FailVm("GROUP_BY multi-aggregate: COUNT(*) output column name must be non-empty");
                        CountStarCol = *Cn;
                        SpecEnd = IdxAfterSpecs + 1;
                    } else if(inst.Operands.size() != IdxAfterSpecs)
                        FailVm("GROUP_BY multi-aggregate bad counts");
                } else if(inst.Operands.size() != IdxAfterSpecs)
                    FailVm("GROUP_BY multi-aggregate: unexpected trailing operands");
                if(inst.Operands.size() != SpecEnd)
                    FailVm("GROUP_BY multi-aggregate: unexpected trailing operands");
                struct AggSpecVm {
                    int Kind = 0;
                    std::string SrcCol;
                    std::string OutCol;
                };
                std::vector<AggSpecVm> Specs;
                Specs.reserve(static_cast<size_t>(*Na));
                size_t Idx = Base + 2;
                for(int64_t A = 0; A < *Na; ++A) {
                    const auto *Knd = std::get_if<int64_t>(&inst.Operands[Idx++]);
                    const auto *Sc = std::get_if<std::string>(&inst.Operands[Idx++]);
                    const auto *Ou = std::get_if<std::string>(&inst.Operands[Idx++]);
                    if(!Knd || !Sc || !Ou || Sc->empty() || Ou->empty())
                        FailVm("GROUP_BY multi-aggregate bad spec");
                    Specs.push_back(AggSpecVm{static_cast<int>(*Knd), *Sc, *Ou});
                }
                struct Accum {
                    std::unordered_map<std::string, Database::Item> KeyTemplate;
                    std::unordered_map<std::string, int64_t> CntStar;
                    std::unordered_map<std::string, std::vector<double>> Sum;
                    std::unordered_map<std::string, std::vector<int64_t>> AvgN;
                    std::unordered_map<std::string, std::vector<bool>> HaveMinMax;
                    std::unordered_map<std::string, std::vector<std::string>> CurMin;
                    std::unordered_map<std::string, std::vector<std::string>> CurMax;
                } Acc;
                for(const auto &Row : Tbl) {
                    const std::string Sig = GroupKeySignature(Row, Keys);
                    if(IncludeCountStar)
                        Acc.CntStar[Sig]++;
                    if(Acc.KeyTemplate.find(Sig) == Acc.KeyTemplate.end()) {
                        Database::Item R;
                        for(const auto &K : Keys) {
                            auto It = Row.find(K);
                            R[K] = It == Row.end() ? "" : It->second;
                        }
                        Acc.KeyTemplate.emplace(Sig, std::move(R));
                    }
                    if(Acc.Sum.find(Sig) == Acc.Sum.end()) {
                        Acc.Sum[Sig] = std::vector<double>(Specs.size(), 0.0);
                        Acc.AvgN[Sig] = std::vector<int64_t>(Specs.size(), 0);
                        Acc.HaveMinMax[Sig] = std::vector<bool>(Specs.size(), false);
                        Acc.CurMin[Sig] = std::vector<std::string>(Specs.size());
                        Acc.CurMax[Sig] = std::vector<std::string>(Specs.size());
                    }
                    auto &Sv = Acc.Sum[Sig];
                    auto &Nv = Acc.AvgN[Sig];
                    auto &Hm = Acc.HaveMinMax[Sig];
                    auto &Cmin = Acc.CurMin[Sig];
                    auto &Cmax = Acc.CurMax[Sig];
                    for(size_t Si = 0; Si < Specs.size(); ++Si) {
                        const auto &Sp = Specs[Si];
                        auto It = Row.find(Sp.SrcCol);
                        const std::string Cell = It == Row.end() ? "" : It->second;
                        switch(Sp.Kind) {
                            case static_cast<int>(GroupCombAggKind::Sum):
                            case static_cast<int>(GroupCombAggKind::Avg): {
                                double X = 0;
                                bool Ok = false;
                                try {
                                    X = std::stod(Cell);
                                    Ok = true;
                                } catch(...) {
                                }
                                if(Ok) {
                                    Sv[Si] += X;
                                    if(Sp.Kind == static_cast<int>(GroupCombAggKind::Avg))
                                        Nv[Si]++;
                                }
                            } break;
                            case static_cast<int>(GroupCombAggKind::Min): {
                                if(!Hm[Si]) {
                                    Hm[Si] = true;
                                    Cmin[Si] = Cell;
                                } else if(CompareScalars(Cell, Cmin[Si]) < 0)
                                    Cmin[Si] = Cell;
                            } break;
                            case static_cast<int>(GroupCombAggKind::Max): {
                                if(!Hm[Si]) {
                                    Hm[Si] = true;
                                    Cmax[Si] = Cell;
                                } else if(CompareScalars(Cell, Cmax[Si]) > 0)
                                    Cmax[Si] = Cell;
                            } break;
                            default:
                                break;
                        }
                    }
                }
                Database::Table OutTbl;
                OutTbl.reserve(Acc.KeyTemplate.size());
                for(auto &Ky : Acc.KeyTemplate) {
                    const std::string &Sig = Ky.first;
                    Database::Item R = Ky.second;
                    if(IncludeCountStar) {
                        const auto ItCnt = Acc.CntStar.find(Sig);
                        R[CountStarCol] = ItCnt == Acc.CntStar.end() ? "0" : std::to_string(ItCnt->second);
                    }
                    const auto &Sv = Acc.Sum.at(Sig);
                    const auto &Nv = Acc.AvgN.at(Sig);
                    const auto &Hm = Acc.HaveMinMax.at(Sig);
                    const auto &Mn = Acc.CurMin.at(Sig);
                    const auto &Mx = Acc.CurMax.at(Sig);
                    for(size_t Si = 0; Si < Specs.size(); ++Si) {
                        const auto &Sp = Specs[Si];
                        switch(Sp.Kind) {
                            case static_cast<int>(GroupCombAggKind::Sum):
                                R[Sp.OutCol] = std::to_string(static_cast<long long>(std::llround(Sv[Si])));
                                break;
                            case static_cast<int>(GroupCombAggKind::Avg):
                                if(Nv[Si] > 0) {
                                    std::ostringstream O;
                                    O << (Sv[Si] / static_cast<double>(Nv[Si]));
                                    R[Sp.OutCol] = O.str();
                                } else
                                    R[Sp.OutCol] = "0";
                                break;
                            case static_cast<int>(GroupCombAggKind::Min):
                                R[Sp.OutCol] = Hm[Si] ? Mn[Si] : "";
                                break;
                            case static_cast<int>(GroupCombAggKind::Max):
                                R[Sp.OutCol] = Hm[Si] ? Mx[Si] : "";
                                break;
                            default:
                                R[Sp.OutCol] = "";
                                break;
                        }
                    }
                    OutTbl.push_back(std::move(R));
                }
                Tbl = std::move(OutTbl);
                std::sort(Tbl.begin(), Tbl.end(), [&](const Database::Item &A, const Database::Item &B) {
                    return GroupKeySignature(A, Keys) < GroupKeySignature(B, Keys);
                });
            } else if(AggMode == 4) {
                const size_t Base = static_cast<size_t>(2 + *Nk);
                if(inst.Operands.size() != Base + 1 && inst.Operands.size() != Base + 2)
                    FailVm("GROUP_BY COUNT(DISTINCT): bad operand count");
                const auto *DistCol = std::get_if<std::string>(&inst.Operands[Base]);
                if(!DistCol || DistCol->empty())
                    FailVm("GROUP_BY COUNT(DISTINCT): bad column name");
                std::string CountStarCol = "cnt";
                if(inst.Operands.size() == Base + 2) {
                    const auto *Cn = std::get_if<std::string>(&inst.Operands[Base + 1]);
                    if(!Cn || Cn->empty())
                        FailVm("GROUP_BY COUNT(DISTINCT): bad COUNT output column name");
                    CountStarCol = *Cn;
                }
                std::unordered_map<std::string, Database::Item> Template;
                std::unordered_map<std::string, std::unordered_set<std::string>> DistinctVals;
                Template.reserve(Tbl.size());
                for(const auto &Row : Tbl) {
                    const std::string Sig = GroupKeySignature(Row, Keys);
                    if(Template.find(Sig) == Template.end()) {
                        Database::Item R;
                        for(const auto &K : Keys) {
                            auto It = Row.find(K);
                            R[K] = It == Row.end() ? "" : It->second;
                        }
                        Template.emplace(Sig, std::move(R));
                    }
                    auto Itv = Row.find(*DistCol);
                    const std::string V = Itv == Row.end() ? "" : Itv->second;
                    DistinctVals[Sig].insert(V);
                }
                Database::Table Out;
                Out.reserve(Template.size());
                for(const auto &Ky : Template) {
                    const std::string &Sig = Ky.first;
                    Database::Item RowOut = Ky.second;
                    const auto ItS = DistinctVals.find(Sig);
                    const std::size_t N = ItS == DistinctVals.end() ? 0 : ItS->second.size();
                    RowOut[CountStarCol] = std::to_string(static_cast<unsigned long long>(N));
                    Out.push_back(std::move(RowOut));
                }
                Tbl = std::move(Out);
                std::sort(Tbl.begin(), Tbl.end(), [&](const Database::Item &A, const Database::Item &B) {
                    return GroupKeySignature(A, Keys) < GroupKeySignature(B, Keys);
                });
            } else if(AggMode == 0) {
                std::unordered_map<std::string, Database::Item> First;
                First.reserve(Tbl.size());
                for(const auto &Row : Tbl) {
                    const std::string Sig = GroupKeySignature(Row, Keys);
                    if(First.find(Sig) == First.end())
                        First.emplace(Sig, Row);
                }
                Database::Table Out;
                Out.reserve(First.size());
                for(auto &P : First)
                    Out.push_back(std::move(P.second));
                Tbl = std::move(Out);
                std::sort(Tbl.begin(), Tbl.end(), [&](const Database::Item &A, const Database::Item &B) {
                    return GroupKeySignature(A, Keys) < GroupKeySignature(B, Keys);
                });
            } else if(AggMode == 1) {
                const size_t Base = static_cast<size_t>(2 + *Nk);
                std::string CountStarCol = "cnt";
                if(inst.Operands.size() == Base + 1) {
                    const auto *Cn = std::get_if<std::string>(&inst.Operands[Base]);
                    if(!Cn || Cn->empty())
                        FailVm("GROUP_BY COUNT(*): bad output column name operand");
                    CountStarCol = *Cn;
                } else if(inst.Operands.size() != Base)
                    FailVm("GROUP_BY COUNT(*): operand count mismatch");
                std::unordered_map<std::string, int64_t> Cnt;
                std::unordered_map<std::string, Database::Item> Template;
                Cnt.reserve(Tbl.size());
                Template.reserve(Tbl.size());
                for(const auto &Row : Tbl) {
                    const std::string Sig = GroupKeySignature(Row, Keys);
                    Cnt[Sig]++;
                    if(Template.find(Sig) == Template.end()) {
                        Database::Item R;
                        for(const auto &K : Keys) {
                            auto It = Row.find(K);
                            R[K] = It == Row.end() ? "" : It->second;
                        }
                        R[CountStarCol] = "0";
                        Template.emplace(Sig, std::move(R));
                    }
                }
                Database::Table Out;
                Out.reserve(Cnt.size());
                for(auto &P : Cnt) {
                    Database::Item RowOut = Template[P.first];
                    RowOut[CountStarCol] = std::to_string(P.second);
                    Out.push_back(std::move(RowOut));
                }
                Tbl = std::move(Out);
                std::sort(Tbl.begin(), Tbl.end(), [&](const Database::Item &A, const Database::Item &B) {
                    return GroupKeySignature(A, Keys) < GroupKeySignature(B, Keys);
                });
            } else
                RunGroupByCore(Tbl, inst, Keys);
            });
            PushOwningStringHeap(new std::string(TableName));
            ++Ic;
            break;
        }
        case Opcode::CLONE_TABLE: {
            if(inst.Operands.size() < 2)
                FailVm("CLONE_TABLE expects dest and src table operands");
            const auto *Dest = std::get_if<std::string>(&inst.Operands[0]);
            const auto *Src = std::get_if<std::string>(&inst.Operands[1]);
            if(!Dest || !Src || Dest->empty() || Src->empty())
                FailVm("CLONE_TABLE operands must be non-empty strings");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->CloneTable(*Dest, *Src);
            ++Ic;
            break;
        }
        case Opcode::WINDOW_ROW_NUMBER: {
            if(inst.Operands.size() < 4)
                FailVm(
                    "WINDOW_ROW_NUMBER expects PARTITION column count plus ORDER/output operands (extended layout)");
            const auto *NpPtr = std::get_if<int64_t>(&inst.Operands[0]);
            if(!NpPtr || *NpPtr < 0 ||
               static_cast<size_t>(*NpPtr) > Limits::MaxWindowPartitionColumns)
                FailVm("WINDOW_ROW_NUMBER invalid PARTITION column count operand");
            const size_t NP = static_cast<size_t>(*NpPtr);
            if(inst.Operands.size() != NP + size_t{4} && inst.Operands.size() != NP + size_t{5} &&
               inst.Operands.size() != NP + size_t{8} && inst.Operands.size() != NP + size_t{12})
                FailVm("WINDOW_ROW_NUMBER operand payload length mismatches PARTITION count");
            int OrdKind = 0;
            std::string SrcCol;
            int64_t FrameOffset = 1;
            if(inst.Operands.size() >= NP + size_t{8}) {
                const auto *Ok = std::get_if<int64_t>(&inst.Operands[NP + 4]);
                const auto *Sc = std::get_if<std::string>(&inst.Operands[NP + 5]);
                const auto *Fo = std::get_if<int64_t>(&inst.Operands[NP + 6]);
                if(!Ok || *Ok < 0 || *Ok > 8)
                    FailVm("WINDOW_ROW_NUMBER bad window kind operand");
                if(!Sc || !Fo || *Fo < 0)
                    FailVm("WINDOW_ROW_NUMBER bad source column or frame offset");
                OrdKind = static_cast<int>(*Ok);
                SrcCol = *Sc;
                FrameOffset = *Fo;
                (void)inst.Operands[NP + 7];
            } else if(inst.Operands.size() == NP + size_t{5}) {
                const auto *Ok = std::get_if<int64_t>(&inst.Operands[NP + 4]);
                if(!Ok || *Ok < 0 || *Ok > 2)
                    FailVm("WINDOW_ROW_NUMBER bad ordinal kind (expected 0=ROW_NUMBER, 1=RANK, 2=DENSE_RANK)");
                OrdKind = static_cast<int>(*Ok);
            }
            std::vector<std::string> PartCols;
            PartCols.reserve(NP);
            for(size_t K = 0; K < NP; ++K) {
                auto *Cn = std::get_if<std::string>(&inst.Operands[K + 1]);
                if(!Cn || Cn->empty())
                    FailVm("WINDOW_ROW_NUMBER PARTITION column operand must be a non-empty string");
                PartCols.push_back(*Cn);
            }
            auto *OrdCol = std::get_if<std::string>(&inst.Operands[NP + 1]);
            auto *AscFl = std::get_if<int64_t>(&inst.Operands[NP + 2]);
            auto *OutCol = std::get_if<std::string>(&inst.Operands[NP + 3]);
            if(!OrdCol || OrdCol->empty() || !AscFl || !OutCol || OutCol->empty())
                FailVm("WINDOW_ROW_NUMBER bad ORDER/output operands");
            const bool Ascending = *AscFl != 0;
            if(StackSlots_.empty())
                FailVm("WINDOW_ROW_NUMBER expects table name on stack");
            std::string WinTable = PopOwnedStringMoved("WINDOW_ROW_NUMBER table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *DbWin = Databases_[0].get();
            const std::string &OC = *OrdCol;
            /** Pre-fetch duplicate PARTITION keys in bytecode operands (cheap sanity gate). */
            {
                std::unordered_set<std::string> SeenP;
                for(const auto &P : PartCols) {
                    if(!SeenP.insert(P).second)
                        FailVm("WINDOW_ROW_NUMBER duplicate PARTITION BY column in bytecode: \"" + P + "\"");
                }
            }
            DbWin->WithExclusiveBytecodeLock([&]() {
                auto &WT = DbWin->Tables_[WinTable];
            const auto OrdLess = [&](const Database::Item &A, const Database::Item &B) -> bool {
                auto Ia = A.find(OC);
                auto Ib = B.find(OC);
                if(Ia == A.end() && Ib == B.end())
                    return false;
                if(Ia == A.end())
                    return Ascending;
                if(Ib == B.end())
                    return !Ascending;
                return Ascending ? Ia->second < Ib->second : Ia->second > Ib->second;
            };
            const auto PartTripleCmp = [&](const Database::Item &A, const Database::Item &B) -> int {
                for(const auto &Cn : PartCols) {
                    const auto Ia = A.find(Cn);
                    const auto Ib = B.find(Cn);
                    const std::string Va = Ia == A.end() ? "" : Ia->second;
                    const std::string Vb = Ib == B.end() ? "" : Ib->second;
                    if(Va != Vb)
                        return Va < Vb ? -1 : 1;
                }
                return 0;
            };
            std::vector<size_t> Order(WT.size());
            std::iota(Order.begin(), Order.end(), size_t{0});
            std::stable_sort(Order.begin(), Order.end(),
                             [&](size_t Ia, size_t Ib) -> bool {
				                 const Database::Item &A = WT[Ia];
				                 const Database::Item &B = WT[Ib];
				                 if(NP) {
					                 const int Cp = PartTripleCmp(A, B);
					                 if(Cp != 0)
						                 return Cp < 0;
				                 }
				                 return OrdLess(A, B);
			                 });
            Database::Table Reordered;
            Reordered.reserve(WT.size());
            for(size_t Idx : Order)
                Reordered.push_back(WT[Idx]);
            WT.swap(Reordered);
            /** Debug (temporary): log after PARTITION+ORDER permutation is materialized into row-major order. */
            if(Logger_) {
                std::ostringstream D;
                D << "[WINDOW_ROW_NUMBER debug] table=\"" << WinTable << "\" rows=" << WT.size()
                  << " partition_col_count=" << NP << " order_by=\"" << OC << "\" ascending=" << (Ascending ? "1" : "0")
                  << " out=\"" << *OutCol << "\" ordinal_kind=" << OrdKind;
                if(NP && !WT.empty()) {
                    D << " first_sorted_row_partition=(";
                    for(size_t Px = 0; Px < NP; ++Px) {
                        if(Px)
                            D << ',';
                        auto It = WT[0].find(PartCols[Px]);
                        D << "\"" << PartCols[Px] << "\"=" << (It == WT[0].end() ? std::string("<null>") : It->second);
                    }
                    D << ")";
                }
                Logger_->Info(D.str());
            }
            std::string PrevPartSig;
            std::string PrevOrdVal;
            bool HavePrevOrd = false;
            long long RowNumInPart = 0;
            long long RankVal = 0;
            long long DenseVal = 0;
            for(size_t R = 0; R < WT.size(); ++R) {
                std::string PartSig;
                if(NP > 0) {
                    std::ostringstream Sig;
                    for(const auto &Cn : PartCols) {
                        Sig << '\0';
                        if(auto It = WT[R].find(Cn); It != WT[R].end())
                            Sig << It->second;
                    }
                    PartSig = std::move(Sig).str();
                }
                auto Io = WT[R].find(OC);
                const std::string OrdVal = Io == WT[R].end() ? "" : Io->second;
                const bool NewPart = NP == 0 ? (R == 0) : (R == 0 || PartSig != PrevPartSig);
                if(NewPart) {
                    RowNumInPart = 0;
                    RankVal = 0;
                    DenseVal = 0;
                    HavePrevOrd = false;
                    if(NP > 0)
                        PrevPartSig = PartSig;
                }
                ++RowNumInPart;
                if(OrdKind == 0)
                    WT[R][*OutCol] = std::to_string(RowNumInPart);
                else if(OrdKind == 1) {
                    if(!HavePrevOrd || OrdVal != PrevOrdVal)
                        RankVal = RowNumInPart;
                    WT[R][*OutCol] = std::to_string(RankVal);
                } else {
                    if(!HavePrevOrd || OrdVal != PrevOrdVal)
                        ++DenseVal;
                    WT[R][*OutCol] = std::to_string(DenseVal);
                }
                PrevOrdVal = OrdVal;
                HavePrevOrd = true;
            }
            });
            PushOwningStringHeap(new std::string(WinTable));
            ++Ic;
            break;
        }
        case Opcode::BEGIN: {
            if (Databases_.empty()) {
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            }
            std::filesystem::path snapshotPath = Databases_[0]->DbPath_;
            snapshotPath += ".txn.snap";
            Databases_[0]->SyncToFileAndCopyMainDbFileTo(snapshotPath);
            Savepoints_["__transaction__"] = snapshotPath.string();
            if(Logger_) Logger_->Info("Transaction started (snapshot)");
            ++Ic;
            break;
        }
        case Opcode::COMMIT: {
            if (auto it = Savepoints_.find("__transaction__"); it != Savepoints_.end()) {
				std::error_code Ec;
				std::filesystem::remove(std::filesystem::path(it->second), Ec);
                Savepoints_.erase(it);
                if(Logger_) Logger_->Info("Transaction committed");
            }
            if(!Databases_.empty())
				Databases_[0]->FlushWalToDisk();
            ++Ic;
            break;
        }
        case Opcode::ROLLBACK: {
            if (auto it = Savepoints_.find("__transaction__"); it != Savepoints_.end()) {
                std::filesystem::path snapshotPath = it->second;
                Savepoints_.erase(it);
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
				}
                if (std::filesystem::exists(snapshotPath)) {
					try {
						Databases_[0]->QuiesceBackgroundIOForFilesystemRollback();
						Databases_[0]->ClearDirtyForFilesystemRollback();
						VmCopyWholeFileOverwrite(snapshotPath, Databases_[0]->DbPath_);
					} catch(const std::exception &Err) {
						FailVm(std::string("ROLLBACK copy failed: ") + Err.what());
					}
					RemoveWalAdjacent(Databases_[0]->DbPath_);
					ReloadPrimaryDatabaseFromDisk();
                }
                if(Logger_) Logger_->Info("Transaction rolled back");
            }
            ++Ic;
            break;
        }
        case Opcode::LIMIT: {
            if (inst.Operands.empty()) FailVm("LIMIT requires count operand");
            if (auto Count = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*Count < 0) FailVm("LIMIT count must be non-negative");
                if (StackSlots_.empty()) FailVm("LIMIT requires table name on stack");
                std::string TableName = PopOwnedStringMoved("LIMIT table");
                
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                Databases_[0]->WithExclusiveBytecodeLock([&]() {
                    auto& Table = Databases_[0]->Tables_[TableName];
                    const std::size_t NewSize = static_cast<std::size_t>(*Count);
                    if (NewSize < Table.size())
                        Table.resize(NewSize);
                });
                PushOwningStringHeap(new std::string(TableName));
            } else {
                FailVm("LIMIT expects integer operand");
            }
            ++Ic;
            break;
        }
        case Opcode::OFFSET: {
            if (inst.Operands.empty()) FailVm("OFFSET requires count operand");
            if (auto Count = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*Count < 0) FailVm("OFFSET must be non-negative");
                if (StackSlots_.empty()) FailVm("OFFSET requires table name on stack");
                std::string TableName = PopOwnedStringMoved("OFFSET table");
                
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                Databases_[0]->WithExclusiveBytecodeLock([&]() {
                    auto& Table = Databases_[0]->Tables_[TableName];
                    const std::size_t Skip = static_cast<std::size_t>(*Count);
                    if(Skip == 0) {
                    } else if (Skip < Table.size()) {
                        Table.erase(Table.begin(),
                                    Table.begin() + static_cast<typename Database::Table::difference_type>(Skip));
                    } else {
                        Table.clear();
                    }
                });
                if(static_cast<std::size_t>(*Count) == 0) {
                    PushOwningStringHeap(new std::string(TableName));
                    ++Ic;
                    break;
                }
                PushOwningStringHeap(new std::string(TableName));
            } else {
                FailVm("OFFSET expects integer operand");
            }
            ++Ic;
            break;
        }
        case Opcode::SLICE_RANGE: {
            if(inst.Operands.size() < 2)
                FailVm("SLICE_RANGE expects offset and limit operands");
            const auto *OffOp = std::get_if<int64_t>(&inst.Operands[0]);
            const auto *LimOp = std::get_if<int64_t>(&inst.Operands[1]);
            if(!OffOp || !LimOp)
                FailVm("SLICE_RANGE expects integer operands");
            if(*OffOp < 0 || *LimOp < 0)
                FailVm("SLICE_RANGE offset and limit must be non-negative");
            if(StackSlots_.empty())
                FailVm("Slicing requires table name on stack");
            std::string TableName = PopOwnedStringMoved("SLICE_RANGE table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->WithExclusiveBytecodeLock([&]() {
                auto &Table = Databases_[0]->Tables_[TableName];
                if(static_cast<size_t>(*OffOp) >= Table.size())
                    Table.clear();
                else {
                    if(*OffOp > 0)
                        Table.erase(Table.begin(), Table.begin() + *OffOp);
                    if(static_cast<size_t>(*LimOp) < Table.size())
                        Table.resize(static_cast<size_t>(*LimOp));
                }
            });
            PushOwningStringHeap(new std::string(TableName));
            ++Ic;
            break;
        }
        case Opcode::CASE_EVAL: {
            /** Layout: out_col, arm_count \e N (may be 0 for ELSE-only), then \e N × (blob, then_kind, then_payload),
             *  then else_kind, else_payload. Minimum size: 4 when \e N = 0. */
            if(inst.Operands.size() < 4)
                FailVm("CASE_EVAL: operand payload too short");
            const auto *OutCol = std::get_if<std::string>(&inst.Operands[0]);
            const auto *Nc = std::get_if<int64_t>(&inst.Operands[1]);
            if(!OutCol || OutCol->empty() || !Nc || *Nc < 0 ||
               static_cast<size_t>(*Nc) > Limits::MaxCaseWhenArms)
                FailVm("CASE_EVAL: bad header (output column / arm count)");
            const size_t Need = static_cast<size_t>(4 + static_cast<size_t>(*Nc) * 3);
            if(inst.Operands.size() != Need)
                FailVm("CASE_EVAL: operand count does not match arm count");

            std::vector<std::vector<std::vector<RowTriple>>> ArmDnfs;
            std::vector<std::pair<int64_t, std::string>> ThenVals;
            ArmDnfs.reserve(static_cast<size_t>(*Nc));
            ThenVals.reserve(static_cast<size_t>(*Nc));

            size_t Oi = 2;
            for(int64_t A = 0; A < *Nc; ++A) {
                const auto *Blob = std::get_if<std::string>(&inst.Operands[Oi++]);
                const auto *Tk = std::get_if<int64_t>(&inst.Operands[Oi++]);
                const auto *Tv = std::get_if<std::string>(&inst.Operands[Oi++]);
                if(!Blob || !Tk || !Tv)
                    FailVm("CASE_EVAL: bad WHEN blob / THEN operands");
				std::vector<std::vector<RowTriple>> Dnf;
				if(!UnpackDnfBlobToBranches(*Blob, Dnf))
					FailVm("CASE_EVAL: corrupt WHEN predicate encoding");
				if(*Tk < 0 || *Tk > 2)
					FailVm("CASE_EVAL: bad THEN kind");
				ArmDnfs.push_back(std::move(Dnf));
				ThenVals.emplace_back(*Tk, *Tv);
            }
            const auto *Ek = std::get_if<int64_t>(&inst.Operands[Oi++]);
            const auto *Ev = std::get_if<std::string>(&inst.Operands[Oi++]);
            if(Oi != inst.Operands.size() || !Ek || !Ev)
                FailVm("CASE_EVAL: bad ELSE operands");
            if(*Ek < 0 || *Ek > 3)
                FailVm("CASE_EVAL: bad ELSE kind");

            if(StackSlots_.empty())
                FailVm("CASE_EVAL: expected table name on stack");
            std::string Tab = PopOwnedStringMoved("CASE_EVAL table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
            Db->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = Db->Tables_[Tab];
                for(Database::Item &Row : Tbl) {
                    bool Hit = false;
                    for(size_t Ai = 0; Ai < ArmDnfs.size(); ++Ai) {
                        if(MatchWhereDnf(Db, Row, ArmDnfs[Ai])) {
                            const auto &[Tk, Payload] = ThenVals[Ai];
                            AssignCaseOutputColumn(Row, *OutCol, Tk, Payload);
                            Hit = true;
                            break;
                        }
                    }
                    if(!Hit) {
                        if(*Ek == 3)
                            Row.erase(*OutCol);
                        else
                            AssignCaseOutputColumn(Row, *OutCol, *Ek, *Ev);
                    }
                }
            });

            PushOwningStringHeap(new std::string(std::move(Tab)));
            ++Ic;
            break;
        }
        case Opcode::CAST_EVAL: {
            if(inst.Operands.size() != 4)
                FailVm("CAST_EVAL expects exactly four operands");
            const auto *OutCol = std::get_if<std::string>(&inst.Operands[0]);
            const auto *SrcK = std::get_if<int64_t>(&inst.Operands[1]);
            const auto *SrcPay = std::get_if<std::string>(&inst.Operands[2]);
            const auto *TgtK = std::get_if<int64_t>(&inst.Operands[3]);
            if(!OutCol || OutCol->empty() || !SrcK || !SrcPay || !TgtK)
                FailVm("CAST_EVAL: bad operand types");
            if(*SrcK < 0 || *SrcK > 2)
                FailVm("CAST_EVAL: bad source scalar kind");
            if(*TgtK < 0 || *TgtK > 3)
                FailVm("CAST_EVAL: bad cast target kind");
            const SqlCastTarget Tgt = static_cast<SqlCastTarget>(*TgtK);

            if(StackSlots_.empty())
                FailVm("CAST_EVAL: expected table name on stack");
            std::string Tab = PopOwnedStringMoved("CAST_EVAL table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = Databases_[0]->Tables_[Tab];
                for(Database::Item &Row : Tbl) {
                    const std::optional<std::string> In = ReadCastProjectionSource(Row, *SrcK, *SrcPay);
                    const std::optional<std::string> OutV = SqlApplyCast(In, Tgt);
                    if(!OutV.has_value())
                        Row.erase(*OutCol);
                    else
                        Row[*OutCol] = *OutV;
                }
            });

            PushOwningStringHeap(new std::string(std::move(Tab)));
            ++Ic;
            break;
        }
        case Opcode::SCALAR_FUNC_EVAL: {
            if(inst.Operands.size() < 3)
                FailVm("SCALAR_FUNC_EVAL expects output column, function tag, and argc");
            const auto *OutCol = std::get_if<std::string>(&inst.Operands[0]);
            const auto *FnTag = std::get_if<int64_t>(&inst.Operands[1]);
            const auto *Argc = std::get_if<int64_t>(&inst.Operands[2]);
            if(!OutCol || OutCol->empty() || !FnTag || !Argc || *Argc < 0 || *Argc > Limits::MaxScalarSqlFuncArgs)
                FailVm("SCALAR_FUNC_EVAL: bad header operands");
            const size_t Need = 3 + static_cast<size_t>(*Argc) * 2;
            if(inst.Operands.size() != Need)
                FailVm("SCALAR_FUNC_EVAL: operand count does not match argc");
            if(*FnTag < 0 || *FnTag > static_cast<int64_t>(ScalarSqlFn::Grouping))
                FailVm("SCALAR_FUNC_EVAL: bad function tag");
            const ScalarSqlFn Fn = static_cast<ScalarSqlFn>(*FnTag);
            size_t Idx = 3;
            std::vector<std::pair<int64_t, std::string>> ArgOps;
            ArgOps.reserve(static_cast<size_t>(*Argc));
            for(int64_t A = 0; A < *Argc; ++A) {
                const auto *Kind = std::get_if<int64_t>(&inst.Operands[Idx++]);
                const auto *Pay = std::get_if<std::string>(&inst.Operands[Idx++]);
                if(!Kind || !Pay || *Kind < 0 || *Kind > 2)
                    FailVm("SCALAR_FUNC_EVAL: bad argument encoding");
                ArgOps.emplace_back(*Kind, *Pay);
            }
            if(StackSlots_.empty())
                FailVm("SCALAR_FUNC_EVAL: expected table name on stack");
            std::string Tab = PopOwnedStringMoved("SCALAR_FUNC_EVAL table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = Databases_[0]->Tables_[Tab];
                for(Database::Item &Row : Tbl) {
                    std::vector<std::string> Cells;
                    Cells.reserve(ArgOps.size());
                    for(const auto &[Kind, Pay] : ArgOps) {
                        const auto V = ReadCastProjectionSource(Row, Kind, Pay);
                        Cells.push_back(V.value_or(std::string()));
                    }
                    const std::optional<std::string> OutV = EvalScalarSqlFn(Fn, Cells, Row);
                    if(!OutV.has_value())
                        Row.erase(*OutCol);
                    else
                        Row[*OutCol] = *OutV;
                }
            });
            PushOwningStringHeap(new std::string(std::move(Tab)));
            ++Ic;
            break;
        }
        case Opcode::KEEP_ROWS: {
            ++Ic;
            break;
        }
        case Opcode::FILTER_DNF: {
            std::vector<std::vector<RowTriple>> Branches;
            size_t End = 0;
            if(!ReadDnfOperands(inst.Operands, 0, End, Branches) || End != inst.Operands.size())
                FailVm("FILTER_DNF bad operand encoding");
            if(StackSlots_.empty())
                FailVm("FILTER_DNF expects table name on stack");
            std::string TableName = PopOwnedStringMoved("FILTER_DNF table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
            Db->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = Db->Tables_[TableName];
                Tbl.erase(std::remove_if(Tbl.begin(), Tbl.end(),
                         [&](const Database::Item &Row) { return !MatchWhereDnf(Db, Row, Branches); }),
                    Tbl.end());
            });
            PushOwningStringHeap(new std::string(TableName));
            ++Ic;
            break;
        }
        case Opcode::DEDUP_ROWS: {
            if(StackSlots_.empty())
                FailVm("DEDUP_ROWS expects table name on stack");
            std::string TableName = PopOwnedStringMoved("DEDUP_ROWS table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = Databases_[0]->Tables_[TableName];
                std::unordered_set<std::string> Seen;
                Database::Table Out;
                for(const auto &Row : Tbl) {
                    const std::string Sig = RowSignatureCanon(Row);
                    if(Seen.insert(Sig).second)
                        Out.push_back(Row);
                }
                Tbl = std::move(Out);
            });
            PushOwningStringHeap(new std::string(TableName));
            ++Ic;
            break;
        }
        case Opcode::SET_COMBINE: {
            if(inst.Operands.size() < 5)
                FailVm("SET_COMBINE: missing operands");
            const auto *Dest = std::get_if<std::string>(&inst.Operands[0]);
            const auto *LhsNm = std::get_if<std::string>(&inst.Operands[1]);
            const auto *RhsNm = std::get_if<std::string>(&inst.Operands[2]);
            const auto *ModeV = std::get_if<int64_t>(&inst.Operands[3]);
            const auto *NcV = std::get_if<int64_t>(&inst.Operands[4]);
            if(!Dest || !LhsNm || !RhsNm || !ModeV || !NcV)
                FailVm("SET_COMBINE: bad header operands");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();

            const int64_t Mode = *ModeV;
            const int64_t NC = *NcV;
            if(NC < 1 || NC > 256)
                FailVm("SET_COMBINE: invalid column count.");
            const size_t Base = 5;
            const size_t Need =
                Base + static_cast<size_t>(NC) * 3;
            if(inst.Operands.size() < Need)
                FailVm("SET_COMBINE: truncated column name payload.");

            std::vector<std::string> OutNames;
            std::vector<std::string> Lsrc;
            std::vector<std::string> Rsrc;
            OutNames.reserve(static_cast<size_t>(NC));
            Lsrc.reserve(static_cast<size_t>(NC));
            Rsrc.reserve(static_cast<size_t>(NC));
            for(int64_t I = 0; I < NC; ++I) {
                const auto *S1 = std::get_if<std::string>(&inst.Operands[Base + static_cast<size_t>(I)]);
                if(!S1 || S1->empty())
                    FailVm("SET_COMBINE: bad output column name.");
                OutNames.push_back(*S1);
            }
            for(int64_t I = 0; I < NC; ++I) {
                const auto *S2 = std::get_if<std::string>(
                    &inst.Operands[Base + static_cast<size_t>(NC + I)]);
                if(!S2 || S2->empty())
                    FailVm("SET_COMBINE: bad LHS projection column.");
                Lsrc.push_back(*S2);
            }
            for(int64_t I = 0; I < NC; ++I) {
                const auto *S3 =
                    std::get_if<std::string>(&inst.Operands[Base + static_cast<size_t>(2 * NC + I)]);
                if(!S3 || S3->empty())
                    FailVm("SET_COMBINE: bad RHS projection column.");
                Rsrc.push_back(*S3);
            }

            Database::Table LtCopy;
            Database::Table RtCopy;
            bool DestExists = false;
            Db->WithExclusiveBytecodeLock([&]() {
                auto ItL = Db->Tables_.find(*LhsNm);
                auto ItR = Db->Tables_.find(*RhsNm);
                if(ItL == Db->Tables_.end() || ItR == Db->Tables_.end())
                    FailVm("SET_COMBINE: LHS or RHS table not found.");
                LtCopy = ItL->second;
                RtCopy = ItR->second;
                DestExists = Db->TableSchemaAssumeDbMutexHeld(*Dest).has_value();
            });

            const Database::Table &LT = LtCopy;
            const Database::Table &RT = RtCopy;

            Database::Table Built;
            if(Mode == static_cast<int64_t>(CompoundSetOpKind::UnionAll)) {
                Built.reserve(LT.size() + RT.size());
                for(const auto &Row : LT)
                    Built.push_back(MapRowByColumnList(Row, Lsrc, OutNames));
                for(const auto &Row : RT)
                    Built.push_back(MapRowByColumnList(Row, Rsrc, OutNames));
            } else if(Mode == static_cast<int64_t>(CompoundSetOpKind::UnionDistinct)) {
                std::unordered_set<std::string> Seen;
                Built.reserve(LT.size() + RT.size());
                for(const auto &Row : LT) {
                    auto P = MapRowByColumnList(Row, Lsrc, OutNames);
                    const std::string Sig = RowSignatureCanon(P);
                    if(Seen.insert(Sig).second)
                        Built.push_back(std::move(P));
                }
                for(const auto &Row : RT) {
                    auto P = MapRowByColumnList(Row, Rsrc, OutNames);
                    const std::string Sig = RowSignatureCanon(P);
                    if(Seen.insert(Sig).second)
                        Built.push_back(std::move(P));
                }
            } else if(Mode == static_cast<int64_t>(CompoundSetOpKind::Intersect)) {
                std::unordered_set<std::string> RhsSigs;
                for(const auto &Row : RT)
                    RhsSigs.insert(RowSignatureCanon(MapRowByColumnList(Row, Rsrc, OutNames)));
                std::unordered_set<std::string> OutSeen;
                for(const auto &Row : LT) {
                    auto P = MapRowByColumnList(Row, Lsrc, OutNames);
                    const std::string Sig = RowSignatureCanon(P);
                    if(RhsSigs.count(Sig) && OutSeen.insert(Sig).second)
                        Built.push_back(std::move(P));
                }
            } else if(Mode == static_cast<int64_t>(CompoundSetOpKind::Except)) {
                std::unordered_set<std::string> RhsSigs;
                for(const auto &Row : RT)
                    RhsSigs.insert(RowSignatureCanon(MapRowByColumnList(Row, Rsrc, OutNames)));
                std::unordered_set<std::string> OutSeen;
                for(const auto &Row : LT) {
                    auto P = MapRowByColumnList(Row, Lsrc, OutNames);
                    const std::string Sig = RowSignatureCanon(P);
                    if(!RhsSigs.count(Sig) && OutSeen.insert(Sig).second)
                        Built.push_back(std::move(P));
                }
            } else
                FailVm("SET_COMBINE: unknown set operation mode.");

            if(DestExists)
                Db->DropTable(*Dest).get();
            Database::Schema Sch;
            for(const std::string &Cn : OutNames) {
                Database::Column C;
                C.Name = Cn;
                C.DefaultValue = "TEXT";
                Sch.push_back(std::move(C));
            }
            Db->CreateTable(*Dest, Sch).get();
            for(auto &R : Built)
                Db->Insert(*Dest, std::move(R)).get();

            ++Ic;
            break;
        }
        case Opcode::DELETE_MATCHING: {
            if(inst.Operands.empty())
                FailVm("DELETE_MATCHING: missing operands");
            auto *TableNm = std::get_if<std::string>(&inst.Operands[0]);
            if(!TableNm)
                FailVm("DELETE_MATCHING expects table name");
            std::vector<std::vector<RowTriple>> Branches;
            size_t End = 0;
			if(!ReadDnfOperands(inst.Operands, 1, End, Branches) || End != inst.Operands.size())
				FailVm("DELETE_MATCHING bad DNF payload");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			Database *Db = Databases_[0].get();
			Db->Delete(*TableNm, [&](const Database::Item &Row) { return MatchWhereDnf(Db, Row, Branches); }).get();
            ++Ic;
            break;
        }
        case Opcode::UPDATE_MATCHING: {
            if(inst.Operands.size() < 3)
                FailVm("UPDATE_MATCHING: missing operands");
            auto *TableNm = std::get_if<std::string>(&inst.Operands[0]);
            auto *Na = std::get_if<int64_t>(&inst.Operands[1]);
            if(!TableNm || !Na || *Na < 0 || *Na > 64)
                FailVm("UPDATE_MATCHING: bad assignment count");
            size_t Oi = 2;
            const size_t NeedAssign = static_cast<size_t>(*Na) * 2;
            if(Oi + NeedAssign > inst.Operands.size())
                FailVm("UPDATE_MATCHING: truncated assignments");
            std::vector<std::pair<std::string, std::string>> Assignments;
            Assignments.reserve(static_cast<size_t>(*Na));
            for(size_t K = 0; K < static_cast<size_t>(*Na); ++K) {
                auto *Cn = std::get_if<std::string>(&inst.Operands[Oi++]);
                auto *Blob = std::get_if<std::string>(&inst.Operands[Oi++]);
                if(!Cn || !Blob)
                    FailVm("UPDATE_MATCHING assignment must be column/expression");
                Assignments.emplace_back(*Cn, *Blob);
            }
            std::vector<std::vector<RowTriple>> Branches;
            size_t End = 0;
            if(!ReadDnfOperands(inst.Operands, Oi, End, Branches) || End != inst.Operands.size())
				FailVm("UPDATE_MATCHING bad DNF payload");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
            Db->UpdateWithSetExprs(*TableNm,
                                   [&](const Database::Item &Row) { return MatchWhereDnf(Db, Row, Branches); },
                                   Assignments)
                .get();
            ++Ic;
            break;
        }
        // Logical/comparison opcodes
        case Opcode::AND: {
            uint64_t b = PopScalarWord("VM AND");
            uint64_t a = PopScalarWord("VM AND");
            PushScalarWord((a && b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::OR: {
            uint64_t b = PopScalarWord("VM OR");
            uint64_t a = PopScalarWord("VM OR");
            PushScalarWord((a || b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::NOT: {
            uint64_t a = PopScalarWord("VM NOT");
            PushScalarWord(!a ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::EQ: {
            uint64_t b = PopScalarWord("VM EQ");
            uint64_t a = PopScalarWord("VM EQ");
            PushScalarWord((a == b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::NE: {
            uint64_t b = PopScalarWord("VM NE");
            uint64_t a = PopScalarWord("VM NE");
            PushScalarWord((a != b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::LT: {
            uint64_t b = PopScalarWord("VM LT");
            uint64_t a = PopScalarWord("VM LT");
            PushScalarWord((a < b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::LE: {
            uint64_t b = PopScalarWord("VM LE");
            uint64_t a = PopScalarWord("VM LE");
            PushScalarWord((a <= b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::GT: {
            uint64_t b = PopScalarWord("VM GT");
            uint64_t a = PopScalarWord("VM GT");
            PushScalarWord((a > b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::GE: {
            uint64_t b = PopScalarWord("VM GE");
            uint64_t a = PopScalarWord("VM GE");
            PushScalarWord((a >= b) ? 1 : 0);
            ++Ic;
            break;
        }
        // LOAD/STORE (registers)
        case Opcode::LOAD: {
            if (inst.Operands.empty()) FailVm("LOAD requires register index operand");
            if (auto reg = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*reg < 0 || static_cast<size_t>(*reg) >= Registers_.size())
                    FailVm("LOAD register index out of range");
                PushScalarWord(Registers_[*reg]);
            } else {
                FailVm("LOAD expects int64_t operand");
            }
            ++Ic;
            break;
        }
        case Opcode::STORE: {
            if (inst.Operands.empty()) FailVm("STORE requires register index operand");
            if (auto reg = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*reg < 0 || static_cast<size_t>(*reg) >= Registers_.size())
                    FailVm("STORE register index out of range");
                Registers_[*reg] = PopScalarWord("STORE");
            } else {
                FailVm("STORE expects int64_t operand");
            }
            ++Ic;
            break;
        }
        // GRANT/REVOKE (permissions)
        case Opcode::GRANT: {
            if(inst.Operands.size() < 2)
                FailVm("GRANT requires grantee and permission operands");
            if(auto Grantee = std::get_if<std::string>(&inst.Operands[0])) {
                if(auto Perms = std::get_if<int64_t>(&inst.Operands[1])) {
                    if(Databases_.empty())
                        Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                    std::string TableName;
                    if(inst.Operands.size() > 2)
                        if(auto Tn = std::get_if<std::string>(&inst.Operands[2]))
                            TableName = *Tn;
                    const bool ToRole =
                        inst.Operands.size() > 3 && std::get_if<int64_t>(&inst.Operands[3]) &&
                        *std::get_if<int64_t>(&inst.Operands[3]) != 0;
                    if(ToRole)
                        Databases_[0]->GrantRolePermission(*Grantee, static_cast<Permissions>(*Perms), TableName).get();
                    else
                        Databases_[0]->GrantPermission(*Grantee, static_cast<Permissions>(*Perms), TableName).get();
                } else {
                    FailVm("GRANT expects int64_t permission operand");
                }
            } else {
                FailVm("GRANT expects string grantee operand");
            }
            ++Ic;
            break;
        }
        case Opcode::REVOKE: {
            if(inst.Operands.size() < 2)
                FailVm("REVOKE requires grantee and permission operands");
            if(auto Grantee = std::get_if<std::string>(&inst.Operands[0])) {
                if(auto Perms = std::get_if<int64_t>(&inst.Operands[1])) {
                    if(Databases_.empty())
                        Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                    std::string TableName;
                    if(inst.Operands.size() > 2)
                        if(auto Tn = std::get_if<std::string>(&inst.Operands[2]))
                            TableName = *Tn;
                    const bool FromRole =
                        inst.Operands.size() > 3 && std::get_if<int64_t>(&inst.Operands[3]) &&
                        *std::get_if<int64_t>(&inst.Operands[3]) != 0;
                    if(FromRole)
                        Databases_[0]->RevokeRolePermission(*Grantee, static_cast<Permissions>(*Perms), TableName).get();
                    else
                        Databases_[0]->RevokePermission(*Grantee, static_cast<Permissions>(*Perms), TableName).get();
                } else {
                    FailVm("REVOKE expects int64_t permission operand");
                }
            } else {
                FailVm("REVOKE expects string grantee operand");
            }
            ++Ic;
            break;
        }
        case Opcode::GRANT_COLUMN: {
            if(inst.Operands.size() < 4)
                FailVm("GRANT_COLUMN requires grantee, permission, table, column operands");
            if(auto Grantee = std::get_if<std::string>(&inst.Operands[0])) {
                if(auto Perms = std::get_if<int64_t>(&inst.Operands[1])) {
                    if(auto TableName = std::get_if<std::string>(&inst.Operands[2])) {
                        if(auto Col = std::get_if<std::string>(&inst.Operands[3])) {
                            if(Databases_.empty())
                                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                            RowColPermission Rule;
                            Rule.Table = *TableName;
                            Rule.Column = *Col;
                            Rule.Perms = static_cast<Permissions>(*Perms);
                            Databases_[0]->GrantRowPermission(*Grantee, Rule).get();
                        } else
                            FailVm("GRANT_COLUMN expects string column operand");
                    } else
                        FailVm("GRANT_COLUMN expects string table operand");
                } else
                    FailVm("GRANT_COLUMN expects int64_t permission operand");
            } else
                FailVm("GRANT_COLUMN expects string grantee operand");
            ++Ic;
            break;
        }
        case Opcode::REVOKE_COLUMN: {
            if(inst.Operands.size() < 4)
                FailVm("REVOKE_COLUMN requires grantee, permission, table, column operands");
            if(auto Grantee = std::get_if<std::string>(&inst.Operands[0])) {
                if(auto Perms = std::get_if<int64_t>(&inst.Operands[1])) {
                    if(auto TableName = std::get_if<std::string>(&inst.Operands[2])) {
                        if(auto Col = std::get_if<std::string>(&inst.Operands[3])) {
                            if(Databases_.empty())
                                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                            RowColPermission Rule;
                            Rule.Table = *TableName;
                            Rule.Column = *Col;
                            Rule.Perms = static_cast<Permissions>(*Perms);
                            Databases_[0]->RevokeRowPermission(*Grantee, Rule).get();
                        } else
                            FailVm("REVOKE_COLUMN expects string column operand");
                    } else
                        FailVm("REVOKE_COLUMN expects string table operand");
                } else
                    FailVm("REVOKE_COLUMN expects int64_t permission operand");
            } else
                FailVm("REVOKE_COLUMN expects string grantee operand");
            ++Ic;
            break;
        }
        case Opcode::CREATE_ROLE: {
            if(inst.Operands.empty())
                FailVm("CREATE_ROLE requires role name operand");
            if(auto Role = std::get_if<std::string>(&inst.Operands[0])) {
                if(Databases_.empty())
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                Databases_[0]->CreateRole(*Role).get();
            } else
                FailVm("CREATE_ROLE expects string operand");
            ++Ic;
            break;
        }
        case Opcode::DROP_ROLE: {
            if(inst.Operands.empty())
                FailVm("DROP_ROLE requires role name operand");
            if(auto Role = std::get_if<std::string>(&inst.Operands[0])) {
                if(Databases_.empty())
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                Databases_[0]->DropRole(*Role).get();
            } else
                FailVm("DROP_ROLE expects string operand");
            ++Ic;
            break;
        }
        case Opcode::GRANT_ROLE_MEMBERSHIP: {
            if(inst.Operands.size() < 2)
                FailVm("GRANT_ROLE_MEMBERSHIP requires role and user operands");
            if(auto Role = std::get_if<std::string>(&inst.Operands[0])) {
                if(auto User = std::get_if<std::string>(&inst.Operands[1])) {
                    if(Databases_.empty())
                        Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                    Databases_[0]->GrantRoleToUser(*Role, *User).get();
                } else
                    FailVm("GRANT_ROLE_MEMBERSHIP expects string user operand");
            } else
                FailVm("GRANT_ROLE_MEMBERSHIP expects string role operand");
            ++Ic;
            break;
        }
        case Opcode::REVOKE_ROLE_MEMBERSHIP: {
            if(inst.Operands.size() < 2)
                FailVm("REVOKE_ROLE_MEMBERSHIP requires role and user operands");
            if(auto Role = std::get_if<std::string>(&inst.Operands[0])) {
                if(auto User = std::get_if<std::string>(&inst.Operands[1])) {
                    if(Databases_.empty())
                        Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                    Databases_[0]->RevokeRoleFromUser(*Role, *User).get();
                } else
                    FailVm("REVOKE_ROLE_MEMBERSHIP expects string user operand");
            } else
                FailVm("REVOKE_ROLE_MEMBERSHIP expects string role operand");
            ++Ic;
            break;
        }
        // JOIN operations
        case Opcode::INNER_JOIN:
        case Opcode::LEFT_JOIN:
        case Opcode::RIGHT_JOIN:
        case Opcode::FULL_JOIN:
        case Opcode::CROSS_JOIN: {
            if(inst.Operands.size() < 4)
                FailVm(
				    "JOIN expects dest table, left table, right table, and ON pair count operands");
            const auto *Dest = std::get_if<std::string>(&inst.Operands[0]);
            const auto *Lt = std::get_if<std::string>(&inst.Operands[1]);
            const auto *Rt = std::get_if<std::string>(&inst.Operands[2]);
            const auto *Npc = std::get_if<int64_t>(&inst.Operands[3]);
            if(!Dest || !Lt || !Rt || !Npc || Dest->empty() || Lt->empty() || Rt->empty())
                FailVm("JOIN dest/left/right must be non-empty strings");
            if(*Npc < 0 || *Npc > 64)
                FailVm("JOIN ON pair count out of range");
            const size_t NeedOps = 4u + 2u * static_cast<size_t>(*Npc);
            if(inst.Operands.size() < NeedOps)
                FailVm("JOIN ON pairs truncated vs count");
            if(inst.Opcode != Opcode::CROSS_JOIN && *Npc == 0)
                FailVm(
				    "JOIN uses ON equality columns unless CROSS JOIN (pair count must be positive)");
            std::vector<std::pair<std::string, std::string>> Pairs;
            for(int64_t Pi = 0; Pi < *Npc; ++Pi) {
                const auto *A = std::get_if<std::string>(&inst.Operands[static_cast<size_t>(4 + 2 * Pi)]);
                const auto *B = std::get_if<std::string>(&inst.Operands[static_cast<size_t>(5 + 2 * Pi)]);
                if(!A || !B)
                    FailVm("JOIN ON requires string column names");
                Pairs.emplace_back(*A, *B);
            }
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *DbPtr = Databases_[0].get();
            Database::Table Result;
            const bool IsCross = inst.Opcode == Opcode::CROSS_JOIN;
            DbPtr->WithExclusiveBytecodeLock([&]() {
                auto &Left = DbPtr->Tables_[*Lt];
                auto &Right = DbPtr->Tables_[*Rt];
                const auto LeftSch = DbPtr->TableSchemaAssumeDbMutexHeld(*Lt);
                const auto RightSch = DbPtr->TableSchemaAssumeDbMutexHeld(*Rt);
                const auto LeftBlank = CollectBlankColumnKeysFromSchema(LeftSch, Left);
                const auto RightBlank = CollectBlankColumnKeysFromSchema(RightSch, Right);

            if(inst.Opcode == Opcode::RIGHT_JOIN) {
                for(const auto &Rr : Right) {
                    bool AnyPair = false;
                    for(const auto &Lr : Left) {
                        if(IsCross || JoinOnMatchPairs(Lr, Rr, Pairs)) {
                            Result.push_back(MergeJoinRowsPreferLeft(Lr, Rr));
                            AnyPair = true;
                        }
                    }
                    if(!AnyPair) {
                        Database::Item Pad;
                        for(const auto &K : LeftBlank)
                            Pad[K] = "";
                        Result.push_back(MergeJoinRowsPreferLeft(Pad, Rr));
                    }
                }
            } else if(inst.Opcode == Opcode::INNER_JOIN || inst.Opcode == Opcode::LEFT_JOIN ||
                      inst.Opcode == Opcode::FULL_JOIN || inst.Opcode == Opcode::CROSS_JOIN) {
                for(const auto &Lr : Left) {
                    bool Any = false;
                    for(const auto &Rr : Right) {
                        if(IsCross || JoinOnMatchPairs(Lr, Rr, Pairs)) {
                            Result.push_back(MergeJoinRowsPreferLeft(Lr, Rr));
                            Any = true;
                        }
                    }
                    if(!Any && (inst.Opcode == Opcode::LEFT_JOIN || inst.Opcode == Opcode::FULL_JOIN)) {
                        Database::Item J = Lr;
                        for(const auto &K : RightBlank) {
                            if(J.find(K) == J.end())
                                J[K] = "";
                        }
                        Result.push_back(std::move(J));
                    }
                }
            }
            if(inst.Opcode == Opcode::FULL_JOIN) {
                for(const auto &Rr : Right) {
                    bool AnyInner = false;
                    for(const auto &Lr : Left) {
                        if(IsCross || JoinOnMatchPairs(Lr, Rr, Pairs)) {
                            AnyInner = true;
                            break;
                        }
                    }
                    if(!AnyInner) {
                        Database::Item Pad;
                        for(const auto &K : LeftBlank)
                            Pad[K] = "";
                        Result.push_back(MergeJoinRowsPreferLeft(Pad, Rr));
                    }
                }
            }
            DbPtr->Tables_[*Dest] = std::move(Result);
            });
            PushOwningStringHeap(new std::string(*Dest));
            ++Ic;
            break;
        }
        // String operations
        case Opcode::CONCAT: {
            if (StackSlots_.size() < 2) FailVm("CONCAT requires two operands");
            std::string b = PopOwnedStringMoved("CONCAT rhs");
            std::string a = PopOwnedStringMoved("CONCAT lhs");
            PushOwningStringHeap(new std::string(a + b));
            ++Ic;
            break;
        }
        case Opcode::SUBSTRING: {
            if (StackSlots_.size() < 3) FailVm("SUBSTRING requires three operands");
            const int64_t length = static_cast<int64_t>(PopScalarWord("SUBSTRING length"));
            const int64_t start = static_cast<int64_t>(PopScalarWord("SUBSTRING start"));
            std::string str = PopOwnedStringMoved("SUBSTRING str");
            PushOwningStringHeap(new std::string(str.substr(static_cast<size_t>(start),
                                                          static_cast<size_t>(length))));
            ++Ic;
            break;
        }
        case Opcode::TRIM:
        case Opcode::LTRIM:
        case Opcode::RTRIM: {
            if (StackSlots_.empty()) FailVm("TRIM requires operand");
            std::string str = PopOwnedStringMoved("TRIM");
            std::string result;
            switch (inst.Opcode) {
                case Opcode::TRIM: result = str; break;
                case Opcode::LTRIM: result = str.substr(str.find_first_not_of(" \t\r\n")); break;
                case Opcode::RTRIM: result = str.substr(0, str.find_last_not_of(" \t\r\n") + 1); break;
                default: break;
            }
            PushOwningStringHeap(new std::string(result));
            ++Ic;
            break;
        }
        // Date/Time operations
        case Opcode::DATE_ADD:
        case Opcode::DATE_SUB: {
            if (StackSlots_.size() < 2) FailVm("DATE operation requires two operands");
            (void)PopOwnedStringMoved("DATE_ADD interval");
            (void)PopOwnedStringMoved("DATE_ADD date");
            // TODO: Implement date arithmetic
            ++Ic;
            break;
        }
        // JSON operations
        case Opcode::JSON_EXTRACT: {
            if (StackSlots_.size() < 2) FailVm("JSON_EXTRACT requires two operands");
            (void)PopOwnedStringMoved("JSON_EXTRACT path");
            (void)PopOwnedStringMoved("JSON_EXTRACT json");
            // TODO: Implement JSON extraction
            ++Ic;
            break;
        }
        // Full-text search
        case Opcode::MATCH:
        case Opcode::AGAINST: {
            if (StackSlots_.size() < 2) FailVm("MATCH/AGAINST requires two operands");
            (void)PopOwnedStringMoved("MATCH term");
            (void)PopOwnedStringMoved("MATCH text");
            // TODO: Implement full-text search
            ++Ic;
            break;
        }
        // Advanced aggregations
        case Opcode::ROLLUP:
        case Opcode::CUBE: {
            if(inst.Operands.size() < 2)
                FailVm("ROLLUP/CUBE expects mode, key count, and key columns");
            const auto *Nk = std::get_if<int64_t>(&inst.Operands[1]);
            if(!Nk || *Nk < 0 || *Nk > 64 || inst.Operands.size() < static_cast<size_t>(2 + *Nk))
                FailVm("ROLLUP/CUBE bad operand layout");
            std::vector<std::string> Keys;
            Keys.reserve(static_cast<size_t>(*Nk));
            for(int64_t I = 0; I < *Nk; ++I) {
                const auto *Ks = std::get_if<std::string>(&inst.Operands[static_cast<size_t>(2 + I)]);
                if(!Ks || Ks->empty())
                    FailVm("ROLLUP/CUBE key must be non-empty string");
                Keys.push_back(*Ks);
            }
            if(StackSlots_.empty())
                FailVm("ROLLUP/CUBE expects table name on stack");
            std::string TableName = PopOwnedStringMoved("ROLLUP/CUBE table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *DbOlap = Databases_[0].get();
            const bool Cube = (inst.Opcode == Opcode::CUBE);
            DbOlap->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = DbOlap->Tables_[TableName];
                RunOlapModifier(Tbl, inst, Keys, Cube);
            });
            PushOwningStringHeap(new std::string(TableName));
            ++Ic;
            break;
        }
        case Opcode::GROUPING_SETS: {
            if(inst.Operands.size() < 2)
                FailVm("GROUPING_SETS expects mode, key count, and key columns");
            const auto *Nk = std::get_if<int64_t>(&inst.Operands[1]);
            if(!Nk || *Nk < 0 || *Nk > 64 || inst.Operands.size() < static_cast<size_t>(2 + *Nk))
                FailVm("GROUPING_SETS bad operand layout");
            std::vector<std::string> Keys;
            Keys.reserve(static_cast<size_t>(*Nk));
            for(int64_t I = 0; I < *Nk; ++I) {
                const auto *Ks = std::get_if<std::string>(&inst.Operands[static_cast<size_t>(2 + I)]);
                if(!Ks || Ks->empty())
                    FailVm("GROUPING_SETS key must be non-empty string");
                Keys.push_back(*Ks);
            }
            if(StackSlots_.empty())
                FailVm("GROUPING_SETS expects table name on stack");
            std::string TableName = PopOwnedStringMoved("GROUPING_SETS table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *DbGs = Databases_[0].get();
            DbGs->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = DbGs->Tables_[TableName];
                RunGroupingSetsOlap(Tbl, inst, Keys);
            });
            PushOwningStringHeap(new std::string(TableName));
            ++Ic;
            break;
        }
        // Subquery support
        case Opcode::EXISTS:
        case Opcode::IN:
        case Opcode::ANY:
        case Opcode::ALL: {
            if (StackSlots_.size() < 2) FailVm("Subquery operation requires two operands");
            
            if (auto subqueryTable = std::get_if<std::string>(&inst.Operands[0])) {
                if(Databases_.empty())
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                static const Database::Table kEmptySubquery;
                bool result = false;
                Databases_[0]->WithExclusiveBytecodeLock([&]() {
                    auto ItSq = Databases_[0]->Tables_.find(*subqueryTable);
                    const Database::Table &subquery =
                        ItSq == Databases_[0]->Tables_.end() ? kEmptySubquery : ItSq->second;
                    switch(inst.Opcode) {
                    case Opcode::EXISTS:
                        result = !subquery.empty();
                        break;
                    case Opcode::IN:
                        if(auto value = std::get_if<std::string>(&inst.Operands[1])) {
                            for(const auto &item : subquery) {
                                for(const auto &[_, val] : item) {
                                    if(val == *value) {
                                        result = true;
                                        break;
                                    }
                                }
                                if(result)
                                    break;
                            }
                        }
                        break;
                    case Opcode::ANY:
                    case Opcode::ALL:
                        if(auto value = std::get_if<std::string>(&inst.Operands[1])) {
                            bool anyMatch = false;
                            bool allMatch = true;
                            for(const auto &item : subquery) {
                                bool itemMatch = false;
                                for(const auto &[_, val] : item) {
                                    if(val == *value) {
                                        itemMatch = true;
                                        break;
                                    }
                                }
                                anyMatch |= itemMatch;
                                allMatch &= itemMatch;
                            }
                            result = (inst.Opcode == Opcode::ANY) ? anyMatch : allMatch;
                        }
                        break;
                    default:
                        break;
                    }
                });
                PushScalarWord(result ? 1 : 0);
            }
            ++Ic;
            break;
        }
        // Transaction control
        case Opcode::SAVEPOINT: {
            if (inst.Operands.empty()) FailVm("SAVEPOINT requires name");
            if (auto name = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                const std::string Slug = SanitizeSavepointSlug(*name);
                std::filesystem::path snapshotPath = Databases_[0]->DbPath_;
                snapshotPath += std::string(".sp.") + Slug + ".snapshot";
                Databases_[0]->SyncToFileAndCopyMainDbFileTo(snapshotPath);
                Savepoints_[*name] = snapshotPath.string();
                if(Logger_) Logger_->Info("SAVEPOINT \"" + *name + "\" captured");
            } else {
                FailVm("SAVEPOINT expects string name operand");
            }
            ++Ic;
            break;
        }
        case Opcode::ROLLBACK_TO: {
            if (inst.Operands.empty()) FailVm("ROLLBACK_TO requires savepoint name");
            if (auto name = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                if (auto it = Savepoints_.find(*name); it != Savepoints_.end()) {
                    const std::filesystem::path snapshotPath = it->second;
                    if (std::filesystem::exists(snapshotPath)) {
						try {
							Databases_[0]->QuiesceBackgroundIOForFilesystemRollback();
							Databases_[0]->ClearDirtyForFilesystemRollback();
							VmCopyWholeFileOverwrite(snapshotPath, Databases_[0]->DbPath_);
						} catch(const std::exception &Err) {
							FailVm(std::string("ROLLBACK TO SAVEPOINT restore failed: ") + Err.what());
						}
                        RemoveWalAdjacent(Databases_[0]->DbPath_);
                        ReloadPrimaryDatabaseFromDisk();
                    }
                    if(Logger_) Logger_->Info("Rolled back to SAVEPOINT \"" + *name + "\"");
                } else if(Logger_) {
                    Logger_->Warn("ROLLBACK TO unknown SAVEPOINT \"" + *name + "\"");
				}
            } else {
                FailVm("ROLLBACK_TO expects string savepoint name operand");
            }
            ++Ic;
            break;
        }
        case Opcode::RELEASE_SAVEPOINT: {
            if (inst.Operands.empty())
                FailVm("RELEASE_SAVEPOINT requires name");
            if (auto Name = std::get_if<std::string>(&inst.Operands[0])) {
				std::error_code Ec;
				if(auto It = Savepoints_.find(*Name); It != Savepoints_.end()) {
					std::filesystem::remove(std::filesystem::path(It->second), Ec);
					Savepoints_.erase(It);
				}
                if(Logger_) Logger_->Info("RELEASE SAVEPOINT \"" + *Name + "\"");
            } else
                FailVm("RELEASE_SAVEPOINT expects string name operand");
            ++Ic;
            break;
        }
		case Opcode::EXPORT_DATABASE: {
			if(inst.Operands.size() < 2)
				FailVm("EXPORT_DATABASE requires path and format operands");
			const auto *Path = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Fmt = std::get_if<std::string>(&inst.Operands[1]);
			if(!Path || !Fmt)
				FailVm("EXPORT_DATABASE expects string path and format");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			if(!Databases_[0]->ExportBundle(*Path, *Fmt))
				FailVm("EXPORT DATABASE failed");
			++Ic;
			break;
		}
		case Opcode::IMPORT_DATABASE: {
			if(inst.Operands.size() < 2)
				FailVm("IMPORT_DATABASE requires path and format operands");
			const auto *Path = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Fmt = std::get_if<std::string>(&inst.Operands[1]);
			if(!Path || !Fmt)
				FailVm("IMPORT_DATABASE expects string path and format");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			if(!Databases_[0]->ImportBundle(*Path, *Fmt))
				FailVm("IMPORT DATABASE failed");
			/* ImportBundle already mutates primary connection in-memory; reloading from disk raced the async
			   flush snapshot and duplicated Database state, provoking heap corruption in later tests on Windows.
			   Interpreter catalog matches live Tables_/schemas after IMPORT without reopen. */
			++Ic;
			break;
		}
		case Opcode::CONVERT_TABULAR_FILES: {
			if(inst.Operands.size() < 4)
				FailVm(
				    "CONVERT_TABULAR_FILES requires src, dst, sourceFormat, destFormat");
			const auto *A = std::get_if<std::string>(&inst.Operands[0]);
			const auto *B = std::get_if<std::string>(&inst.Operands[1]);
			const auto *Sf = std::get_if<std::string>(&inst.Operands[2]);
			const auto *Df = std::get_if<std::string>(&inst.Operands[3]);
			if(!A || !B || !Sf || !Df)
				FailVm("CONVERT_TABULAR_FILES expects four string operands");
			if(!Database::ConvertTabularFiles(*A, *B, *Sf, *Df))
				FailVm("CONVERT failed");
			++Ic;
			break;
		}
        default:
            std::cerr << "Unimplemented opcode: " << static_cast<int>(inst.Opcode) << std::endl;
            FailVm("Unimplemented opcode");
    }
    return true;
}

} // namespace SQL
} // namespace AstralDB
