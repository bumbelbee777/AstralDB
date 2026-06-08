#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bytecode/BytecodeDebug.hxx>
#include <SQL/Procedures/BytecodeProcedures.hxx>
#include <SQL/Procedures/BytecodeTriggers.hxx>
#include <Database/Expression/SetExprEval.hxx>
#include <SQL/Parser/SetExprEval.hxx>
#include <SQL/SQL.hxx>
#include <IO/Limits.hxx>
#include <IO/Error.hxx>
#include <Database/Types/AdvancedTypes.hxx>
#include <Database/MathSci/MathSci.hxx>
#include <Database/MathSci/MathSciComplex.hxx>
#include <Database/MathSci/MathSciEmbeddings.hxx>
#include <Database/Types/JsonCell.hxx>
#include <SQL/Parser/JsonSql.hxx>
#include <SQL/Parser/XmlSql.hxx>
#include <SQL/Bulk/BulkOps.hxx>
#include <SQL/Bulk/BulkDominantAmb.hxx>
#include <SQL/Bulk/BulkDominantWarehouseMegafusion.hxx>
#include <SQL/Shape/QueryShapeRouter.hxx>
#include <SQL/Bytecode/FastPathGuard.hxx>
#include <SQL/Fusion/FusedOps.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/Microkernels.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/BulkSyntheticDerive.hxx>
#include <DS/JSON.hxx>
#include <SQL/Parser/MatchRecognize.hxx>
#include <SQL/Parser/TextSearch.hxx>
#include <SQL/Parser/DialectCompat.hxx>
#include <Database/Index/FtsIndex.hxx>
#include <IO/MathUtil.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/Dataset.hxx>
#include <Database/Graph/Graph.hxx>
#include <Database/Database.hxx>
#include <Database/Storage/Superfetch.hxx>
#include <Database/Storage/TimeSeries.hxx>
#include <IO/SIMD.hxx>
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
#include <chrono>
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

constexpr std::size_t WindowLazyRowMaterializeMax = 16'384;

bool IsDmlBoundaryOpcode(const Opcode Op) noexcept {
	switch(Op) {
	case Opcode::CREATE_TABLE:
	case Opcode::DROP_TABLE:
	case Opcode::CREATE_TYPE:
	case Opcode::DROP_TYPE:
	case Opcode::INSERT:
	case Opcode::INSERT_BULK:
	case Opcode::UPDATE:
	case Opcode::DELETE:
	case Opcode::UPDATE_MATCHING:
	case Opcode::DELETE_MATCHING:
		return true;
	default:
		return false;
	}
}

bool BytecodeIsReadOnlyQuery(const Bytecode &Code) {
	for(const Instruction &Inst : Code) {
		if(IsDmlBoundaryOpcode(Inst.Opcode_))
			return false;
	}
	return true;
}

bool IsSelectFinalizeInst(const Instruction &Inst) noexcept {
	if(Inst.Opcode_ != Opcode::SELECT || Inst.Operands.empty())
		return false;
	return std::get_if<int64_t>(&Inst.Operands[0]) != nullptr;
}

std::size_t FindReadOnlyStatementEnd(const Bytecode &Code, const std::size_t Start) noexcept {
	std::size_t End = Start;
	while(End < Code.size()) {
		if(IsDmlBoundaryOpcode(Code[End].Opcode_))
			break;
		++End;
		if(IsSelectFinalizeInst(Code[End - 1]))
			break;
	}
	return End;
}

bool TryExecuteDominantReadOnlySegment(BytecodeInterpreter &Vm, const Bytecode &Code, const std::size_t Start,
                                       const std::size_t End) {
	if(Start >= End || End > Code.size())
		return false;
	const Bytecode Sub(Code.begin() + static_cast<std::ptrdiff_t>(Start),
	                   Code.begin() + static_cast<std::ptrdiff_t>(End));
	const std::uint64_t ResultBefore = Vm.MutableTimeSqlStats().ResultRows;
	const auto CommitSegment = [&]() {
		Vm.MutableTimeSqlStats().ResultRows = ResultBefore + Vm.MutableTimeSqlStats().ResultRows;
		return true;
	};
	if(TryExecuteReadOnlyViaShapeRouter(Vm, Sub))
		return CommitSegment();
	if(TryExecuteDominantStarJoinGroupBytecode(Vm, Sub))
		return CommitSegment();
	if(TryExecuteDominantStarJoinSelectBytecode(Vm, Sub))
		return CommitSegment();
	if(TryExecuteDominantStarJoinCubeBytecode(Vm, Sub))
		return CommitSegment();
	if(TryExecuteDominantSemistructuredBytecode(Vm, Sub))
		return CommitSegment();
	if(TryExecuteDominantBulkQueryMetadata(Vm, Sub))
		return CommitSegment();
	if(TryExecuteDominantAmbBytecode(Vm, Sub))
		return CommitSegment();
	return false;
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

static size_t ResolveRowsFrameLocalIndex(WindowFrameBoundKind Kind, int64_t Off, size_t LocalIdx,
                                       size_t PartSize) {
	if(PartSize == 0)
		return 0;
	switch(Kind) {
	case WindowFrameBoundKind::UnboundedPreceding:
		return 0;
	case WindowFrameBoundKind::Preceding: {
		const size_t O = static_cast<size_t>(Off < 0 ? 0 : Off);
		return LocalIdx >= O ? LocalIdx - O : 0;
	}
	case WindowFrameBoundKind::CurrentRow:
		return LocalIdx < PartSize ? LocalIdx : PartSize - 1;
	case WindowFrameBoundKind::Following: {
		const size_t O = static_cast<size_t>(Off < 0 ? 0 : Off);
		const size_t T = LocalIdx + O;
		return T < PartSize ? T : PartSize - 1;
	}
	case WindowFrameBoundKind::UnboundedFollowing:
		return PartSize - 1;
	}
	return LocalIdx < PartSize ? LocalIdx : PartSize - 1;
}

static void ApplyDefaultWindowFrame(int OrdKind, WindowFrameBound &FrameStart, WindowFrameBound &FrameEnd) {
	FrameStart = {WindowFrameBoundKind::UnboundedPreceding, 0};
	FrameEnd = {WindowFrameBoundKind::CurrentRow, 0};
	if(OrdKind == static_cast<int>(WindowFnKind::LastValue)) {
		FrameStart = {WindowFrameBoundKind::CurrentRow, 0};
		FrameEnd = {WindowFrameBoundKind::UnboundedFollowing, 0};
	}
}

static std::string FormatUtcTimestamp(int64_t Epoch) {
	std::string Out = TimeSeries::FormatEpochSeconds(Epoch);
	if(!Out.empty())
		Out.push_back('Z');
	return Out;
}

static std::optional<double> TryParseWindowNumeric(const std::string &Cell) {
	if(Cell.empty())
		return std::nullopt;
	try {
		size_t Pos = 0;
		const double D = std::stod(Cell, &Pos);
		if(Pos > 0)
			return D;
	} catch(...) {
	}
	return std::nullopt;
}

static int CompareWindowOrderCells(const std::string &A, const std::string &B, bool Ascending) {
	if(const auto Na = TryParseWindowNumeric(A)) {
		if(const auto Nb = TryParseWindowNumeric(B)) {
			if(*Na < *Nb)
				return Ascending ? -1 : 1;
			if(*Na > *Nb)
				return Ascending ? 1 : -1;
			return 0;
		}
	}
	if(A < B)
		return Ascending ? -1 : 1;
	if(A > B)
		return Ascending ? 1 : -1;
	return 0;
}

static std::optional<int64_t> ParseTimezoneOffsetMinutes(const std::string &S) {
	if(S.empty())
		return std::nullopt;
	bool HasColon = S.find(':') != std::string::npos;
	if((S[0] == '+' || S[0] == '-') && (HasColon || S.size() == 5)) {
		const int Sign = S[0] == '-' ? -1 : 1;
		std::string Hs;
		std::string Ms;
		if(HasColon) {
			const size_t Pos = S.find(':');
			Hs = S.substr(1, Pos - 1);
			Ms = S.substr(Pos + 1);
		} else {
			Hs = S.substr(1, 2);
			Ms = S.substr(3, 2);
		}
		try {
			const int64_t H = std::stoll(Hs);
			const int64_t M = std::stoll(Ms);
			if(M < 0 || M > 59)
				return std::nullopt;
			return Sign * (H * 60 + M);
		} catch(...) {
			return std::nullopt;
		}
	}
	try {
		return std::stoll(S);
	} catch(...) {
	}
	return std::nullopt;
}

static std::pair<size_t, size_t> ResolveRangeFrameLocalBounds(const Database::Table &WT, size_t Pf, size_t Ps,
                                                              size_t LocalIdx, const std::string &OrderCol,
                                                              bool Ascending, WindowFrameBound Start,
                                                              WindowFrameBound End) {
	const auto OrdAt = [&](size_t Local) -> std::string {
		const auto It = WT[Pf + Local].find(OrderCol);
		return It == WT[Pf + Local].end() ? std::string() : It->second;
	};
	const auto OrdLessEq = [&](size_t A, size_t B) -> bool {
		const std::string Va = OrdAt(A);
		const std::string Vb = OrdAt(B);
		if(const auto Na = TryParseWindowNumeric(Va)) {
			if(const auto Nb = TryParseWindowNumeric(Vb))
				return Ascending ? *Na <= *Nb : *Na >= *Nb;
		}
		return Ascending ? Va <= Vb : Va >= Vb;
	};
	const std::string CurOrd = OrdAt(LocalIdx);
	const auto CurNum = TryParseWindowNumeric(CurOrd);
	auto SatisfiesStart = [&](size_t Local) -> bool {
		switch(Start.Kind) {
		case WindowFrameBoundKind::UnboundedPreceding:
			return true;
		case WindowFrameBoundKind::CurrentRow:
			return Local == LocalIdx;
		case WindowFrameBoundKind::Preceding:
			if(!CurNum)
				return Local + Start.Offset >= LocalIdx;
			if(const auto On = TryParseWindowNumeric(OrdAt(Local))) {
				const double Th = *CurNum - static_cast<double>(Start.Offset);
				return Ascending ? *On >= Th : *On <= Th;
			}
			return OrdLessEq(Local, LocalIdx);
		case WindowFrameBoundKind::Following:
			if(!CurNum)
				return Local <= LocalIdx + static_cast<size_t>(Start.Offset);
			if(const auto On = TryParseWindowNumeric(OrdAt(Local))) {
				const double Th = *CurNum + static_cast<double>(Start.Offset);
				return Ascending ? *On >= Th : *On <= Th;
			}
			return OrdLessEq(LocalIdx, Local);
		case WindowFrameBoundKind::UnboundedFollowing:
			return true;
		}
		return false;
	};
	auto SatisfiesEnd = [&](size_t Local) -> bool {
		switch(End.Kind) {
		case WindowFrameBoundKind::UnboundedPreceding:
			return true;
		case WindowFrameBoundKind::CurrentRow:
			return Local == LocalIdx;
		case WindowFrameBoundKind::Preceding:
			if(!CurNum)
				return Local + End.Offset >= LocalIdx;
			if(const auto On = TryParseWindowNumeric(OrdAt(Local))) {
				const double Th = *CurNum - static_cast<double>(End.Offset);
				return Ascending ? *On <= Th : *On >= Th;
			}
			return OrdLessEq(Local, LocalIdx);
		case WindowFrameBoundKind::Following:
			if(!CurNum)
				return Local <= LocalIdx + static_cast<size_t>(End.Offset);
			if(const auto On = TryParseWindowNumeric(OrdAt(Local))) {
				const double Th = *CurNum + static_cast<double>(End.Offset);
				return Ascending ? *On <= Th : *On >= Th;
			}
			return OrdLessEq(LocalIdx, Local);
		case WindowFrameBoundKind::UnboundedFollowing:
			return true;
		}
		return false;
	};
	size_t Lo = LocalIdx;
	size_t Hi = LocalIdx;
	for(size_t L = 0; L <= LocalIdx; ++L) {
		if(SatisfiesStart(L)) {
			Lo = L;
			break;
		}
	}
	for(size_t L = LocalIdx; L < Ps; ++L) {
		if(SatisfiesEnd(L))
			Hi = L;
	}
	return {Lo, Hi};
}

static void WriteWindowAggregate(Database::Item &Row, const std::string &OutCol, int OrdKind,
                                 const Database::Table &WT, size_t Lo, size_t Hi, const std::string &SrcCol) {
	bool Any = false;
	double Sum = 0;
	int64_t AvgN = 0;
	bool HaveMinMax = false;
	std::string MinV;
	std::string MaxV;
	for(size_t J = Lo; J <= Hi && J < WT.size(); ++J) {
		auto It = WT[J].find(SrcCol);
		const std::string Cell = It == WT[J].end() ? "" : It->second;
		switch(OrdKind) {
		case static_cast<int>(WindowFnKind::Sum):
		case static_cast<int>(WindowFnKind::Avg): {
			if(const auto N = TryParseWindowNumeric(Cell)) {
				Sum += *N;
				Any = true;
				if(OrdKind == static_cast<int>(WindowFnKind::Avg))
					++AvgN;
			}
		} break;
		case static_cast<int>(WindowFnKind::Min):
			if(!HaveMinMax) {
				HaveMinMax = true;
				MinV = Cell;
				Any = true;
			} else if(CompareScalars(Cell, MinV) < 0)
				MinV = Cell;
			break;
		case static_cast<int>(WindowFnKind::Max):
			if(!HaveMinMax) {
				HaveMinMax = true;
				MaxV = Cell;
				Any = true;
			} else if(CompareScalars(Cell, MaxV) > 0)
				MaxV = Cell;
			break;
		default:
			break;
		}
	}
	if(!Any) {
		Row.erase(OutCol);
		return;
	}
	switch(OrdKind) {
	case static_cast<int>(WindowFnKind::Sum):
		Row[OutCol] = std::to_string(static_cast<long long>(std::llround(Sum)));
		break;
	case static_cast<int>(WindowFnKind::Avg):
		if(AvgN > 0) {
			std::ostringstream O;
			O << (Sum / static_cast<double>(AvgN));
			Row[OutCol] = O.str();
		} else
			Row.erase(OutCol);
		break;
	case static_cast<int>(WindowFnKind::Min):
		Row[OutCol] = MinV;
		break;
	case static_cast<int>(WindowFnKind::Max):
		Row[OutCol] = MaxV;
		break;
	default:
		break;
	}
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
static constexpr const char *kOpNotIn = "__NOT_IN__";
static constexpr char kInSubPredColBytecode[] = "__ASTRAL_IN_SUBQUERY__";

static bool CellIsSqlNull(const Database::Item &Row, const std::string &Col) {
	auto It = Row.find(Col);
	if(It == Row.end())
		return true;
	const std::string &V = It->second;
	return V.empty() || V == "NULL";
}

static bool SqlCellIsNullValue(std::string_view V) {
	return V.empty() || V == "NULL";
}

/** SQL UNKNOWN: comparisons involving NULL (empty cell) do not satisfy WHERE. */
static bool ComparisonOperandIsNull(const std::string &Lhs, const std::string &Rhs, const std::string &Op) {
	if(Op == kOpIsNull || Op == kOpIsNotNull)
		return false;
	if(SqlCellIsNullValue(Lhs))
		return true;
	if(Op == kOpIn || Op == kOpNotIn)
		return false;
	if(Op == "LIKE" || Op == "NOT LIKE" || Op == "ILIKE" || Op == "NOT ILIKE" || Op == "GLOB" ||
	   Op == "NOT GLOB" || Op == "REGEXP" || Op == "NOT REGEXP" || Op == "REGEXP_ICASE" || Op == "NOT REGEXP_ICASE"
	   || Op == "~" || Op == "!~" || Op == "~*" || Op == "!~*" || Op == "MATCH" || Op == "NOT MATCH")
		return SqlCellIsNullValue(Rhs);
	return SqlCellIsNullValue(Rhs);
}

static bool SqlLikeBounded(const std::string &Str, const std::string &Pat) {
	const size_t n = Str.size(), m = Pat.size();
	const size_t DpRows = m + 1;
	const size_t Cols = n + 1;
	if(DpRows != 0 && Cols != 0) {
		if(DpRows > Limits::MaxSqlLikeDpCells || Cols > Limits::MaxSqlLikeDpCells)
			return false;
		if(DpRows > Limits::MaxSqlLikeDpCells / Cols)
			return false;
	}
	return SqlLikeAscii(Str, Pat);
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
static constexpr char kQuantifiedSubqueryColBytecode[] = "__ASTRAL_QSUBQ__";
static constexpr char kRowCompareColBytecode[] = "__ASTRAL_ROWCMP__";
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

static bool DecodeInSubqueryPayload(const std::string &Rhs, bool &NegOut, std::string &InnerCol,
                                    std::vector<std::vector<RowTriple>> &Inner) {
	const size_t Split = Rhs.find('\x1E');
	if(Split == std::string::npos || Split == 0 || (Rhs[0] != '0' && Rhs[0] != '1'))
		return false;
	NegOut = Rhs[0] == '1';
	const size_t ColEnd = Rhs.find('\x1E', Split + 1);
	if(ColEnd == std::string::npos)
		return false;
	InnerCol.assign(Rhs.data() + Split + 1, ColEnd - Split - 1);
	return DecodePackedDnfOperands(Rhs, ColEnd + 1, Rhs.size(), Inner);
}

static bool MatchWhereDnf(const Database *Db, const std::string &ContextTable, const Database::Item &Row,
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
	for(const auto &InnerRow : Tit->second.RowStore) {
		const Database::Item Combined = MergeForExistsRow(Db, Op, EnclosingRow, InnerRow);
		if(MatchWhereDnf(Db, Op, Combined, Inner)) {
			Any = true;
			break;
		}
	}
	return Neg ? !Any : Any;
}

static std::string FirstInnerColumnValue(const Database *Db, const std::string &InnerRelation,
                                         const Database::Item &InnerRow, const std::string &InnerCol) {
	if(!InnerCol.empty()) {
		auto It = InnerRow.find(InnerCol);
		return It == InnerRow.end() ? std::string() : It->second;
	}
	const auto Sch = Db->TableSchemaAssumeDbMutexHeld(InnerRelation);
	if(Sch && !Sch->empty()) {
		auto It = InnerRow.find(Sch->front().Name);
		return It == InnerRow.end() ? std::string() : It->second;
	}
	for(const auto &KV : InnerRow)
		return KV.second;
	return std::string();
}

static bool DecodeRowExprAtom(const std::string &Serialized, char &Kind, std::string &Payload) {
	if(Serialized.empty())
		return false;
	Kind = Serialized[0];
	size_t Off = 1;
	if(!PullSizedString(Serialized, Off, Payload))
		return false;
	return Off == Serialized.size();
}

static std::optional<std::string> ResolveRowExprAtomValue(const Database::Item &Row, char Kind,
                                                          const std::string &Payload) {
	if(Kind == 'N')
		return std::nullopt;
	if(Kind == 'L')
		return Payload;
	if(Kind == 'C') {
		auto It = Row.find(Payload);
		if(It == Row.end() || It->second.empty())
			return std::nullopt;
		return It->second;
	}
	return std::nullopt;
}

static bool EvaluateRowComparePredicate(const RowTriple &Pred, const Database::Item &Row) {
	const auto &[Col, Op, Rhs] = Pred;
	(void)Col;
	size_t Off = 0;
	int64_t N = 0;
	if(!PullLeI64(Rhs, Off, N) || N <= 0 || N > 64)
		return false;
	int FirstNonEq = 0;
	for(int64_t I = 0; I < N; ++I) {
		std::string LSer;
		std::string RSer;
		if(!PullSizedString(Rhs, Off, LSer) || !PullSizedString(Rhs, Off, RSer))
			return false;
		char Lk = 0;
		char Rk = 0;
		std::string Lp;
		std::string Rp;
		if(!DecodeRowExprAtom(LSer, Lk, Lp) || !DecodeRowExprAtom(RSer, Rk, Rp))
			return false;
		const auto Lv = ResolveRowExprAtomValue(Row, Lk, Lp);
		const auto Rv = ResolveRowExprAtomValue(Row, Rk, Rp);
		if(!Lv.has_value() || !Rv.has_value())
			return false;
		if(FirstNonEq == 0)
			FirstNonEq = CompareScalars(*Lv, *Rv);
	}
	if(Op == "=" || Op == "==")
		return FirstNonEq == 0;
	if(Op == "!=")
		return FirstNonEq != 0;
	if(Op == "<")
		return FirstNonEq < 0;
	if(Op == "<=")
		return FirstNonEq <= 0;
	if(Op == ">")
		return FirstNonEq > 0;
	if(Op == ">=")
		return FirstNonEq >= 0;
	return false;
}

static bool EvaluateQuantifiedSubqueryPredicate(const Database *Db, const RowTriple &Pred,
                                                const Database::Item &EnclosingRow) {
	const auto &[Col, Op, Rhs] = Pred;
	(void)Col;
	if(!Db)
		FailVm("INTERNAL: quantified subquery evaluation requires Database context");
	size_t Off = 0;
	int64_t Qk = 0;
	if(!PullLeI64(Rhs, Off, Qk) || (Qk != 0 && Qk != 1))
		return false;
	std::string LhsSer;
	std::string InnerTable;
	std::string InnerCol;
	std::string DnfBlob;
	if(!PullSizedString(Rhs, Off, LhsSer) || !PullSizedString(Rhs, Off, InnerTable) ||
	   !PullSizedString(Rhs, Off, InnerCol) || !PullSizedString(Rhs, Off, DnfBlob) || Off != Rhs.size())
		return false;
	char Lk = 0;
	std::string Lp;
	if(!DecodeRowExprAtom(LhsSer, Lk, Lp))
		return false;
	const auto LhsVal = ResolveRowExprAtomValue(EnclosingRow, Lk, Lp);
	if(!LhsVal.has_value())
		return false;
	std::vector<std::vector<RowTriple>> Inner;
	if(!DecodePackedDnfOperands(DnfBlob, 0, DnfBlob.size(), Inner))
		return false;
	const auto Tit = Db->Tables_.find(InnerTable);
	if(Tit == Db->Tables_.end())
		return Qk == 1;
	bool SeenAny = false;
	bool AnyMatch = false;
	bool AllMatch = true;
	for(const auto &InnerRow : Tit->second.RowStore) {
		const Database::Item Combined = MergeForExistsRow(Db, InnerTable, EnclosingRow, InnerRow);
		if(!MatchWhereDnf(Db, InnerTable, Combined, Inner))
			continue;
		const std::string InnerVal = FirstInnerColumnValue(Db, InnerTable, InnerRow, InnerCol);
		if(SqlCellIsNullValue(InnerVal))
			continue;
		SeenAny = true;
		const bool Cmp = CellCompare(*LhsVal, InnerVal, Op);
		AnyMatch = AnyMatch || Cmp;
		AllMatch = AllMatch && Cmp;
	}
	if(Qk == 0)
		return AnyMatch;
	if(!SeenAny)
		return true;
	return AllMatch;
}

static bool InSubqueryPredicateHolds(const Database *Db, const RowTriple &Pred, const Database::Item &EnclosingRow) {
	const auto &[LhsCol, Op, Rhs] = Pred;
	(void)Op;
	if(!Db)
		FailVm("INTERNAL: IN subquery evaluation requires Database context");
	const size_t TblSplit = Rhs.find('\x1E');
	if(TblSplit == std::string::npos)
		return false;
	const std::string InnerTable(Rhs.data(), TblSplit);
	const std::string Payload = Rhs.substr(TblSplit + 1);
	bool Neg = false;
	std::string InnerCol;
	std::vector<std::vector<RowTriple>> Inner;
	if(!DecodeInSubqueryPayload(Payload, Neg, InnerCol, Inner))
		return Neg;
	const auto Tit = Db->Tables_.find(InnerTable);
	if(Tit == Db->Tables_.end())
		return Neg;
	auto ItLhs = EnclosingRow.find(LhsCol);
	const std::string LhsVal = ItLhs == EnclosingRow.end() ? "" : ItLhs->second;
	if(SqlCellIsNullValue(LhsVal))
		return false;
	bool Any = false;
	for(const auto &InnerRow : Tit->second.RowStore) {
		const Database::Item Combined = MergeForExistsRow(Db, InnerTable, EnclosingRow, InnerRow);
		if(!MatchWhereDnf(Db, InnerTable, Combined, Inner))
			continue;
		const std::string InnerVal = FirstInnerColumnValue(Db, InnerTable, InnerRow, InnerCol);
		if(CellCompare(LhsVal, InnerVal, "=")) {
			Any = true;
			break;
		}
	}
	return Neg ? !Any : Any;
}

static bool SqlTruthLiteral(std::string_view S) {
	return S == "1" || S == "true" || S == "TRUE" || S == "t" || S == "yes";
}

static bool MatchOnePredicate(const Database *Db, const std::string &ContextTable, const Database::Item &Row,
                              const RowTriple &Pred) {
	(void)ContextTable;
	const auto &[Col, Op, Rhs] = Pred;
	if(Col == kExistPredColBytecode)
		return ExistPredicateHolds(Db, Pred, Row);
	if(Col == kQuantifiedSubqueryColBytecode)
		return EvaluateQuantifiedSubqueryPredicate(Db, Pred, Row);
	if(Col == kRowCompareColBytecode)
		return EvaluateRowComparePredicate(Pred, Row);
	if(Op == kInSubPredColBytecode)
		return InSubqueryPredicateHolds(Db, Pred, Row);
	auto ItCol = Row.find(Col);
	const std::string Lhs = ItCol == Row.end() ? "" : ItCol->second;

	static constexpr size_t kRhsColMarkLen = sizeof(kAstRhsColMarker) - 1;
	if(Op != kOpIsNull && Op != kOpIsNotNull && Op != kOpIn && Op != kOpNotIn && Op != kInSubPredColBytecode &&
	   Rhs.size() >= kRhsColMarkLen &&
	   Rhs.compare(0, kRhsColMarkLen, kAstRhsColMarker, kRhsColMarkLen) == 0) {
		const std::string_view RcolSv(Rhs.data() + kRhsColMarkLen, Rhs.size() - kRhsColMarkLen);
		const std::string Rcol(RcolSv.begin(), RcolSv.end());
		auto ItR = Row.find(Rcol);
		const std::string RhsVal = ItR == Row.end() ? "" : ItR->second;
		if(ComparisonOperandIsNull(Lhs, RhsVal, Op))
			return false;
		if(Op == "LIKE")
			return SqlLikeBounded(Lhs, RhsVal);
		if(Op == "NOT LIKE")
			return !SqlLikeBounded(Lhs, RhsVal);
		if(Op == "ILIKE")
			return SqlLikeAsciiCaseInsensitive(Lhs, RhsVal);
		if(Op == "NOT ILIKE")
			return !SqlLikeAsciiCaseInsensitive(Lhs, RhsVal);
		if(Op == "GLOB")
			return SqlGlobMatch(Lhs, RhsVal);
		if(Op == "NOT GLOB")
			return !SqlGlobMatch(Lhs, RhsVal);
		if(Op == "REGEXP" || Op == "~")
			return SqlRegexpMatch(Lhs, RhsVal, false);
		if(Op == "NOT REGEXP" || Op == "!~")
			return !SqlRegexpMatch(Lhs, RhsVal, false);
		if(Op == "REGEXP_ICASE" || Op == "~*")
			return SqlRegexpMatch(Lhs, RhsVal, true);
		if(Op == "NOT REGEXP_ICASE" || Op == "!~*")
			return !SqlRegexpMatch(Lhs, RhsVal, true);
		if(Op == "MATCH")
			return TextSearch::MatchesQuery(Lhs, RhsVal);
		if(Op == "NOT MATCH")
			return !TextSearch::MatchesQuery(Lhs, RhsVal);
		return CellCompare(Lhs, RhsVal, Op);
	}

	if(Op == kOpIsNull)
		return CellIsSqlNull(Row, Col);
	if(Op == kOpIsNotNull)
		return !CellIsSqlNull(Row, Col);
	if(Op == kOpIn || Op == kOpNotIn) {
		if(SqlCellIsNullValue(Lhs))
			return false;
		std::vector<std::string> Vals;
		if(!SplitInList(Rhs, Vals))
			return false;
		bool Hit = false;
		for(const auto &V : Vals) {
			if(SqlCellIsNullValue(V))
				continue;
			if(CellCompare(Lhs, V, "=")) {
				Hit = true;
				break;
			}
		}
		return Op == kOpNotIn ? !Hit : Hit;
	}
	if(ComparisonOperandIsNull(Lhs, Rhs, Op))
		return false;
	if(Op == "LIKE")
		return SqlLikeBounded(Lhs, Rhs);
	if(Op == "NOT LIKE")
		return !SqlLikeBounded(Lhs, Rhs);
	if(Op == "ILIKE")
		return SqlLikeAsciiCaseInsensitive(Lhs, Rhs);
	if(Op == "NOT ILIKE")
		return !SqlLikeAsciiCaseInsensitive(Lhs, Rhs);
	if(Op == "GLOB")
		return SqlGlobMatch(Lhs, Rhs);
	if(Op == "NOT GLOB")
		return !SqlGlobMatch(Lhs, Rhs);
	if(Op == "REGEXP" || Op == "~")
		return SqlRegexpMatch(Lhs, Rhs, false);
	if(Op == "NOT REGEXP" || Op == "!~")
		return !SqlRegexpMatch(Lhs, Rhs, false);
	if(Op == "REGEXP_ICASE" || Op == "~*")
		return SqlRegexpMatch(Lhs, Rhs, true);
	if(Op == "NOT REGEXP_ICASE" || Op == "!~*")
		return !SqlRegexpMatch(Lhs, Rhs, true);
	if(Op == "MATCH")
		return TextSearch::MatchesQuery(Lhs, Rhs);
	if(Op == "NOT MATCH")
		return !TextSearch::MatchesQuery(Lhs, Rhs);
	if(Op == "JSON_EXTRACT" || Op == "NOT JSON_EXTRACT") {
		const std::size_t Split = Rhs.find('\x1E');
		if(Split == std::string::npos)
			return Op == "NOT JSON_EXTRACT";
		const std::string Path = Rhs.substr(0, Split);
		const std::string Want = Rhs.substr(Split + 1);
		std::string Got;
		if(const auto Root = JsonCell::ParseCellJson(Lhs)) {
			if(const auto Val = JsonCell::ExtractPath(*Root, Path))
				Got = JsonCell::JsonCellToText(*Val);
		}
		if(Got.empty())
			return Op == "NOT JSON_EXTRACT";
		const bool Hit = Got == Want || (SqlTruthLiteral(Want) && SqlTruthLiteral(Got));
		return Op == "NOT JSON_EXTRACT" ? !Hit : Hit;
	}
	if(Op == "XML_VALID" || Op == "NOT XML_VALID") {
		const bool Valid = XmlSql::IsValidXml(Lhs);
		const bool Hit = Valid == SqlTruthLiteral(Rhs);
		return Op == "NOT XML_VALID" ? !Hit : Hit;
	}
	return CellCompare(Lhs, Rhs, Op);
}

static bool MatchBranchConjunction(const Database *Db, const std::string &ContextTable, const Database::Item &Row,
                                  const std::vector<RowTriple> &Branch) {
	for(const auto &P : Branch) {
		if(!MatchOnePredicate(Db, ContextTable, Row, P))
			return false;
	}
	return true;
}

static bool MatchWhereDnf(const Database *Db, const std::string &ContextTable, const Database::Item &Row,
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
		if(MatchBranchConjunction(Db, ContextTable, Row, B))
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
	return MatchWhereDnf(Db, std::string{}, Row, Branches);
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
	if(SrcKind == 0 || SrcKind == 5)
		return Payload;
	if(SrcKind != 1)
		FailVm("CAST_EVAL: bad source operand kind");
	auto It = Row.find(Payload);
	if(It == Row.end() || It->second.empty())
		return std::nullopt;
	return It->second;
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
                                                  const Database::Item &Row, Database *Db) {
	switch(Fn) {
	case ScalarSqlFn::Now: {
		if(!Cells.empty())
			return std::nullopt;
		const auto Now = std::chrono::system_clock::now();
		const auto Sec = std::chrono::duration_cast<std::chrono::seconds>(Now.time_since_epoch()).count();
		return TimeSeries::FormatEpochSeconds(static_cast<int64_t>(Sec));
	}
	case ScalarSqlFn::CurrentDate: {
		if(!Cells.empty())
			return std::nullopt;
		const auto Now = std::chrono::system_clock::now();
		const auto Sec = std::chrono::duration_cast<std::chrono::seconds>(Now.time_since_epoch()).count();
		const std::string Ts = TimeSeries::FormatEpochSeconds(static_cast<int64_t>(Sec));
		return Ts.size() >= 10 ? Ts.substr(0, 10) : Ts;
	}
	case ScalarSqlFn::CurrentTime: {
		if(!Cells.empty())
			return std::nullopt;
		const auto Now = std::chrono::system_clock::now();
		const auto Sec = std::chrono::duration_cast<std::chrono::seconds>(Now.time_since_epoch()).count();
		return TimeSeries::FormatEpochSeconds(static_cast<int64_t>(Sec));
	}
	case ScalarSqlFn::CurrentTimestamp: {
		if(!Cells.empty())
			return std::nullopt;
		const auto Now = std::chrono::system_clock::now();
		const auto Sec = std::chrono::duration_cast<std::chrono::seconds>(Now.time_since_epoch()).count();
		return TimeSeries::FormatEpochSeconds(static_cast<int64_t>(Sec));
	}
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
		const auto P = TimeSeries::ParseIsoYmd(Cells[0]);
		if(!P)
			return std::nullopt;
		if(Fn == ScalarSqlFn::ExtractYear)
			return std::to_string(*P / 10000);
		if(Fn == ScalarSqlFn::ExtractMonth)
			return std::to_string((*P / 100) % 100);
		return std::to_string(*P % 100);
	}
	case ScalarSqlFn::ExtractHour:
	case ScalarSqlFn::ExtractMinute:
	case ScalarSqlFn::ExtractSecond:
	case ScalarSqlFn::ExtractEpoch: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto Ep = TimeSeries::ParseEpochSeconds(Cells[0]);
		if(!Ep)
			return std::nullopt;
		if(Fn == ScalarSqlFn::ExtractEpoch)
			return std::to_string(*Ep);
		const int64_t Rem = *Ep % 86400LL;
		if(Fn == ScalarSqlFn::ExtractHour)
			return std::to_string(Rem / 3600);
		if(Fn == ScalarSqlFn::ExtractMinute)
			return std::to_string((Rem % 3600) / 60);
		return std::to_string(Rem % 60);
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
		const auto Packed = TimeSeries::ParseIsoYmd(Cells[0]);
		if(!Packed)
			return std::nullopt;
		const auto Out = TimeSeries::IsoAddDays(*Packed, Delta);
		return Out ? std::optional<std::string>(TimeSeries::FormatIsoYmd(*Out)) : std::nullopt;
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
		const auto Packed = TimeSeries::ParseIsoYmd(Cells[0]);
		if(!Packed)
			return std::nullopt;
		const auto Out = TimeSeries::IsoAddDays(*Packed, -Delta);
		return Out ? std::optional<std::string>(TimeSeries::FormatIsoYmd(*Out)) : std::nullopt;
	}
	case ScalarSqlFn::DateDiffDays: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto A = TimeSeries::ParseEpochSeconds(Cells[0]);
		const auto B = TimeSeries::ParseEpochSeconds(Cells[1]);
		if(!A || !B)
			return std::nullopt;
		return std::to_string(TimeSeries::EpochDiffDays(*A, *B));
	}
	case ScalarSqlFn::DateAddSeconds: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Ep = TimeSeries::ParseEpochSeconds(Cells[0]);
		if(!Ep)
			return std::nullopt;
		int64_t Delta = 0;
		try {
			Delta = std::stoll(Cells[1]);
		} catch(...) {
			return std::nullopt;
		}
		const auto Out = TimeSeries::EpochAddSeconds(*Ep, Delta);
		return Out ? std::optional<std::string>(TimeSeries::FormatEpochSeconds(*Out)) : std::nullopt;
	}
	case ScalarSqlFn::TimestampDiffSeconds: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto A = TimeSeries::ParseEpochSeconds(Cells[0]);
		const auto B = TimeSeries::ParseEpochSeconds(Cells[1]);
		if(!A || !B)
			return std::nullopt;
		return std::to_string(TimeSeries::EpochDiffSeconds(*A, *B));
	}
	case ScalarSqlFn::TimeBucketSeconds: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Ep = TimeSeries::ParseEpochSeconds(Cells[0]);
		if(!Ep)
			return std::nullopt;
		int64_t Width = 0;
		try {
			Width = std::stoll(Cells[1]);
		} catch(...) {
			return std::nullopt;
		}
		const auto Bucket = TimeSeries::TimeBucketEpoch(*Ep, Width);
		return Bucket ? std::optional<std::string>(TimeSeries::FormatEpochSeconds(*Bucket)) : std::nullopt;
	}
	case ScalarSqlFn::DateTrunc: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Unit = TimeSeries::ParseTruncUnit(Cells[0]);
		const auto Ep = TimeSeries::ParseEpochSeconds(Cells[1]);
		if(!Unit || !Ep)
			return std::nullopt;
		const auto Tr = TimeSeries::TruncateEpoch(*Ep, *Unit);
		return Tr ? std::optional<std::string>(TimeSeries::FormatEpochSeconds(*Tr)) : std::nullopt;
	}
	case ScalarSqlFn::AtTimeZone: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Ep = TimeSeries::ParseEpochSeconds(Cells[0]);
		const auto OffMin = ParseTimezoneOffsetMinutes(Cells[1]);
		if(!Ep || !OffMin)
			return std::nullopt;
		return FormatUtcTimestamp(*Ep + (*OffMin * 60));
	}
	case ScalarSqlFn::ConvertTimezone: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto Ep = TimeSeries::ParseEpochSeconds(Cells[0]);
		const auto FromMin = ParseTimezoneOffsetMinutes(Cells[1]);
		const auto ToMin = ParseTimezoneOffsetMinutes(Cells[2]);
		if(!Ep || !FromMin || !ToMin)
			return std::nullopt;
		const int64_t Shift = (*ToMin - *FromMin) * 60;
		return FormatUtcTimestamp(*Ep + Shift);
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
	case ScalarSqlFn::StructField: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto S = AdvancedTypes::ParseStructCell(Cells[0]);
		if(!S)
			return std::nullopt;
		for(const auto &[K, V] : *S) {
			if(K == Cells[1])
				return V;
		}
		return std::nullopt;
	}
	case ScalarSqlFn::MapGet: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto M = AdvancedTypes::ParseMapCell(Cells[0]);
		if(!M)
			return std::nullopt;
		auto It = M->find(Cells[1]);
		return It == M->end() ? std::nullopt : std::optional<std::string>(It->second);
	}
	case ScalarSqlFn::ComplexReal: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto C = AdvancedTypes::ParseComplexCell(Cells[0]);
		return C ? std::optional<std::string>(std::to_string(C->first)) : std::nullopt;
	}
	case ScalarSqlFn::ComplexImag: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto C = AdvancedTypes::ParseComplexCell(Cells[0]);
		return C ? std::optional<std::string>(std::to_string(C->second)) : std::nullopt;
	}
	case ScalarSqlFn::ComplexMul: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto A = AdvancedTypes::ParseComplexCell(Cells[0]);
		const auto B = AdvancedTypes::ParseComplexCell(Cells[1]);
		if(!A || !B)
			return std::nullopt;
		float ARe = static_cast<float>(A->first);
		float AIm = static_cast<float>(A->second);
		float BRe = static_cast<float>(B->first);
		float BIm = static_cast<float>(B->second);
		float OutRe = 0.f;
		float OutIm = 0.f;
		Simd::ComplexMulF32(&ARe, &AIm, &BRe, &BIm, &OutRe, &OutIm);
		return AdvancedTypes::FormatComplexCell(OutRe, OutIm);
	}
	case ScalarSqlFn::VectorDot: {
		if(Cells.size() != 2)
			return std::nullopt;
		return MathSciComplex::DotCellFromReal(Cells[0], Cells[1]);
	}
	case ScalarSqlFn::VectorAdd: {
		if(Cells.size() != 2)
			return std::nullopt;
		return MathSciComplex::AddCellFromReal(Cells[0], Cells[1]);
	}
	case ScalarSqlFn::VectorNorm: {
		if(Cells.size() != 1)
			return std::nullopt;
		return MathSciComplex::NormCellFromReal(Cells[0]);
	}
	case ScalarSqlFn::MatrixVec: {
		if(Cells.size() != 2)
			return std::nullopt;
		return MathSciComplex::MatVecCellFromReal(Cells[0], Cells[1]);
	}
	case ScalarSqlFn::TextContains: {
		if(Cells.size() != 2)
			return std::nullopt;
		return TextSearch::MatchesQuery(Cells[0], Cells[1]) ? std::string("1") : std::string("0");
	}
	case ScalarSqlFn::TextMatch: {
		if(Cells.size() != 2)
			return std::nullopt;
		return TextSearch::MatchAgainst(Cells[0], Cells[1]) ? std::string("1") : std::string("0");
	}
	case ScalarSqlFn::RegexpMatch:
		if(Cells.size() != 2)
			return std::nullopt;
		return SqlRegexpMatch(Cells[0], Cells[1], false) ? std::string("1") : std::string("0");
	case ScalarSqlFn::GroupingId: {
		int64_t Mask = 0;
		for(size_t I = 0; I < Cells.size(); ++I) {
			const std::string Key = "_grouping_" + Cells[I];
			auto It = Row.find(Key);
			if(It != Row.end() && It->second == "1")
				Mask |= (int64_t{1} << static_cast<int>(I));
		}
		return std::to_string(Mask);
	}
	case ScalarSqlFn::VectorTopK: {
		if(!Db || Cells.size() != 3)
			return std::nullopt;
		const auto Query = AdvancedTypes::ParseVectorCell(Cells[1]);
		if(!Query)
			return AdvancedTypes::FormatListCell({});
		std::size_t K = 1;
		try {
			K = static_cast<std::size_t>(std::stoull(Cells[2]));
		} catch(...) {
			return std::nullopt;
		}
		const auto Rows = Db->VectorTopKAssumeDbMutexHeld(Cells[0], *Query, K);
		std::vector<std::string> Out;
		Out.reserve(Rows.size());
		for(size_t R : Rows)
			Out.push_back(std::to_string(R));
		return AdvancedTypes::FormatListCell(Out);
	}
	default:
		if(const auto R = MathSci::EvalScalar(Fn, Cells, Db))
			return R;
		return std::nullopt;
	}
}

static std::optional<std::string> SqlApplyCast(const std::optional<std::string> &InOpt, SqlCastTarget T,
                                               const std::string &TypeSql) {
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
	case SqlCastTarget::Advanced: {
		if(Trim.empty())
			return std::nullopt;
		const auto Desc = AdvancedTypes::ParseTypeSpelling(TypeSql);
		if(!Desc)
			return std::nullopt;
		return AdvancedTypes::NormalizeCell(*Desc, Trim);
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
		int64_t GroupingBitmap = 0;
		for(size_t Ki = 0; Ki < AllKeys.size(); ++Ki) {
			const auto &K = AllKeys[Ki];
			const bool Active =
			    std::find(ActiveKeys.begin(), ActiveKeys.end(), K) != ActiveKeys.end();
			if(!Active)
				Row[K] = "";
			const bool Grouped = !Active;
			Row["_grouping_" + K] = Grouped ? "1" : "0";
			if(Grouped)
				GroupingBitmap |= (int64_t{1} << static_cast<int>(Ki));
		}
		Row["_olap_level"] = std::to_string(OlapLevel);
		Row["_grouping_id"] = std::to_string(GroupingBitmap);
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
	if(AggMode == 3 && ColumnarGroupBy::TryRun(Tbl, Inst, ActiveKeys))
		return;
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
		Superfetch::RowScanSession GbScan;
		Superfetch::BeginRowScan(Tbl, GbScan);
		for(std::size_t Tri = 0; Tri < Tbl.size(); ++Tri) {
			Superfetch::AdvanceRowScan(Tbl, Tri, GbScan);
			const auto &Row = Tbl[Tri];
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
		Superfetch::RowScanSession GbScan;
		Superfetch::BeginRowScan(Tbl, GbScan);
		for(std::size_t Tri = 0; Tri < Tbl.size(); ++Tri) {
			Superfetch::AdvanceRowScan(Tbl, Tri, GbScan);
			const auto &Row = Tbl[Tri];
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
		Superfetch::RowScanSession GbScan;
		Superfetch::BeginRowScan(Tbl, GbScan);
		for(std::size_t Tri = 0; Tri < Tbl.size(); ++Tri) {
			Superfetch::AdvanceRowScan(Tbl, Tri, GbScan);
			const auto &Row = Tbl[Tri];
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
static Database::Schema MergeJoinSchemas(const std::optional<Database::Schema> &Left,
                                         const std::optional<Database::Schema> &Right) {
	Database::Schema Out;
	if(Left) {
		for(const Database::Column &C : *Left)
			Out.push_back(C);
	}
	if(Right) {
		for(const Database::Column &C : *Right) {
			const bool Dup = std::any_of(Out.begin(), Out.end(),
			                             [&](const Database::Column &E) { return E.Name == C.Name; });
			if(!Dup)
				Out.push_back(C);
		}
	}
	return Out;
}

static std::vector<std::string> CollectBlankColumnKeysFromSchema(const std::optional<Database::Schema> &Sch,
                                                                const Database::Table &RowStore) {
	std::set<std::string> Names;
	if(Sch)
		for(const auto &Col : *Sch)
			Names.insert(Col.Name);
	for(const auto &Row : RowStore) {
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
	    "GENERATED:", "IDENTITY:", "STUB_"};
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
			if(Pipe != std::string::npos) {
				ForeignKey Dfk;
				Dfk.ColumnName = Col.Name;
				Dfk.ReferencedTable = R.substr(0, Pipe);
				Dfk.ReferencedColumn = R.substr(Pipe + 1);
				Col.DeclaredFk = std::move(Dfk);
			}
		} else if(T.rfind("IDENTITY:", 0) == 0) {
			const std::string R = T.substr(9);
			const size_t P1 = R.find(':');
			const size_t P2 = P1 == std::string::npos ? std::string::npos : R.find(':', P1 + 1);
			if(P1 == std::string::npos || P2 == std::string::npos)
				continue;
			Col.IsIdentity = true;
			Col.IdentityAlways = R.substr(0, P1) == "1";
			try {
				Col.IdentityStart = std::stoll(R.substr(P1 + 1, P2 - P1 - 1));
				Col.IdentityIncrement = std::stoll(R.substr(P2 + 1));
			} catch(...) {
				Col.IdentityStart = 1;
				Col.IdentityIncrement = 1;
			}
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
	if(Inst.Opcode_ == Opcode::PUSH) {
		if(const auto *S = std::get_if<std::string>(&Inst.Operands[0]))
			return *S;
	}
	if(Inst.Opcode_ == Opcode::PUSH_POOL) {
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
	if(Idx >= Code.size() || Code[Idx].Opcode_ != Opcode::PUSH)
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

void BytecodeInterpreter::ResetVmState() {
	CleanupStack();
	Registers_.clear();
	Ic = 0;
	Sp = 0;
	Bp = 0;
	Flags = 0;
	StepsExecuted_ = 0;
	StringOperandPool_ = nullptr;
	Savepoints_.clear();
	MemSavepoints_.clear();
	ResetTimeSqlStats();
	SetShapeReadOnlyQueryContext(false);
}

void BytecodeInterpreter::ResetExecutionSession(std::optional<std::filesystem::path> NewDatabasePath,
                                                bool WipeOnDisk) {
	ResetVmState();
	BorrowedPrimary_ = nullptr;
	if(NewDatabasePath)
		DatabasePath_ = *NewDatabasePath;
	if(!Databases_.empty()) {
		for(auto &Db : Databases_) {
			if(WipeOnDisk)
				Db->SetSkipExitSyncOnDestroy(true);
			else
				Db->QuiesceAsyncFsync();
		}
		Databases_.clear();
	}
	if(WipeOnDisk && NewDatabasePath) {
		std::error_code Ec;
		std::filesystem::remove(*NewDatabasePath, Ec);
		RemoveWalAdjacent(*NewDatabasePath);
	}
}

void BytecodeInterpreter::EnsurePrimaryDatabaseOpened() {
	if(BorrowedPrimary_)
		return;
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

void BytecodeInterpreter::RestorePrimaryDatabaseFromSnapshotFile(
    const std::filesystem::path &SnapshotPath) {
	std::filesystem::path DbPath = DatabasePath_;
	Logger *L = Logger_;
	if(!Databases_.empty()) {
		DbPath = Databases_[0]->DbPath_;
		L = Databases_[0]->GetLogger();
		Databases_[0]->QuiesceBackgroundIOForFilesystemRollback();
		Databases_[0]->ClearDirtyForFilesystemRollback();
		Databases_[0]->SetSkipExitSyncOnDestroy(true);
		Databases_.clear();
	}
	VmCopyWholeFileOverwrite(SnapshotPath, DbPath);
	RemoveWalAdjacent(DbPath);
	Databases_.push_back(std::make_unique<Database>(DbPath, L));
}

void BytecodeInterpreter::Execute(const Bytecode &Code) {
	Execute(Code, nullptr);
}

void BytecodeInterpreter::Execute(const CompiledBytecode &Compiled) {
	Execute(Compiled.Instructions,
	        Compiled.StringPool.empty() ? nullptr : &Compiled.StringPool);
}

void BytecodeInterpreter::VmSavepoint(const std::string &Name) {
	if(Databases_.empty())
		Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
	if(Databases_[0]->PreferMemorySnapshots()) {
		MemSavepoints_[Name] = Databases_[0]->CaptureWorkingSnapshot();
		return;
	}
	const std::string Slug = SanitizeSavepointSlug(Name);
	std::filesystem::path SnapshotPath = Databases_[0]->DbPath_;
	SnapshotPath += std::string(".sp.") + Slug + ".snapshot";
	Databases_[0]->SyncToFileAndCopyMainDbFileTo(SnapshotPath);
	Savepoints_[Name] = SnapshotPath.string();
}

void BytecodeInterpreter::VmRollbackToSavepoint(const std::string &Name) {
	if(Databases_.empty())
		Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
	if(const auto Mem = MemSavepoints_.find(Name); Mem != MemSavepoints_.end()) {
		Databases_[0]->RestoreWorkingSnapshot(Mem->second);
		return;
	}
	if(const auto It = Savepoints_.find(Name); It != Savepoints_.end()) {
		const std::filesystem::path SnapshotPath = It->second;
		if(std::filesystem::exists(SnapshotPath))
			RestorePrimaryDatabaseFromSnapshotFile(SnapshotPath);
	}
}

void BytecodeInterpreter::VmReleaseSavepoint(const std::string &Name) {
	MemSavepoints_.erase(Name);
	std::error_code Ec;
	if(const auto It = Savepoints_.find(Name); It != Savepoints_.end()) {
		std::filesystem::remove(std::filesystem::path(It->second), Ec);
		Savepoints_.erase(It);
	}
}

bool BytecodeInterpreter::DispatchProcedureException(const std::runtime_error &Err) {
	auto Matches = [&](const std::string &Cond) -> bool {
		std::string U = Cond;
		FoldAsciiUpper(U);
		std::string Eu = Err.what();
		FoldAsciiUpper(Eu);
		if(U == "OTHERS")
			return true;
		if(U.rfind("SQLSTATE:", 0) == 0)
			return Eu.find(U.substr(9)) != std::string::npos;
		return Eu.find(U) != std::string::npos;
	};
	while(!ProcTryStack_.empty()) {
		ProcTryFrame Frame = std::move(ProcTryStack_.back());
		ProcTryStack_.pop_back();
		VmRollbackToSavepoint(Frame.Savepoint);
		for(const auto &[HandlerIc, Cond] : Frame.Handlers) {
			if(Matches(Cond)) {
				Ic = static_cast<uintptr_t>(HandlerIc);
				return true;
			}
		}
	}
	return false;
}

void BytecodeInterpreter::RunNestedBytecode(const Bytecode &Code, const std::vector<std::string> *StringPool) {
	const uintptr_t OuterIc = Ic;
	const auto *PrevPool = StringOperandPool_;
	StringOperandPool_ = StringPool;
	ProcTryStack_.clear();
	Ic = 0;
	while(Ic < Code.size()) {
		if(DebugSession_ && DebugSession_->Report().HaltedEarly)
			break;
		if(DebugSession_) {
			VmTraceEvent Ev;
			Ev.Ip = static_cast<std::size_t>(Ic);
			Ev.Op = Code[static_cast<std::size_t>(Ic)].Opcode_;
			Ev.StackDepth = StackSlots_.size();
			Ev.StepNumber = StepsExecuted_ + 1;
			DebugSession_->NotifyBeforeStep(Ev);
			if(DebugSession_->Report().HaltedEarly)
				break;
		}
		if(++StepsExecuted_ > Limits::MaxInterpreterSteps)
			FailVm("Nested procedure exceeded the VM step limit.");
		try {
			if(!Step(Code))
				break;
		} catch(const std::runtime_error &E) {
			if(DispatchProcedureException(E))
				continue;
			throw;
		}
	}
	ProcTryStack_.clear();
	StringOperandPool_ = PrevPool;
	Ic = OuterIc + 1;
}

void BytecodeInterpreter::Execute(const Bytecode &Code, const std::vector<std::string> *StringPool) {
	Reset();
	StringOperandPool_ = StringPool;
	StepsExecuted_ = 0;
	if(TryExecuteReadOnlyViaShapeRouter(*this, Code))
		return;
	if(TryExecuteDominantWarehouseMegafusionBytecode(*this, Code))
		return;
	if(TryExecuteDominantStarJoinGroupBytecode(*this, Code))
		return;
	if(TryExecuteDominantStarJoinSelectBytecode(*this, Code))
		return;
	if(TryExecuteDominantStarJoinCubeBytecode(*this, Code))
		return;
	if(TryExecuteDominantSemistructuredBytecode(*this, Code))
		return;
	if(BytecodeIsReadOnlyQuery(Code) && TryExecuteDominantBulkQueryMetadata(*this, Code))
		return;
	if(BytecodeIsReadOnlyQuery(Code) && TryExecuteDominantAmbBytecode(*this, Code))
		return;
	double TelemetryStepMsTotal = 0.0;
	double TelemetryStepMsMax = 0.0;
	std::size_t TelemetryFailures = 0;
	while(Ic < Code.size()) {
		if(DebugSession_ && DebugSession_->Report().HaltedEarly)
			break;
		const std::size_t Cur = static_cast<std::size_t>(Ic);
		if(!IsDmlBoundaryOpcode(Code[Cur].Opcode_)) {
			const std::size_t StmtEnd = FindReadOnlyStatementEnd(Code, Cur);
			if(StmtEnd > Cur && TryExecuteDominantReadOnlySegment(*this, Code, Cur, StmtEnd)) {
				Ic = static_cast<int64_t>(StmtEnd);
				continue;
			}
		}
		if(DebugSession_) {
			VmTraceEvent Ev;
			Ev.Ip = Cur;
			Ev.Op = Code[Cur].Opcode_;
			Ev.StackDepth = StackSlots_.size();
			Ev.StepNumber = StepsExecuted_ + 1;
			DebugSession_->NotifyBeforeStep(Ev);
			if(DebugSession_->Report().HaltedEarly)
				break;
		}
		++StepsExecuted_;
		if((StepsExecuted_ & 63) == 0 && StepsExecuted_ > Limits::MaxInterpreterSteps)
			FailVm("Statement exceeded the VM step limit (safety guard against infinite loops or oversized programs).");
		const auto StepStarted = std::chrono::steady_clock::now();
		try {
			if(!Step(Code))
				break;
		} catch(...) {
			++TelemetryFailures;
			throw;
		}
		const auto StepEnded = std::chrono::steady_clock::now();
		const double StepMs = std::chrono::duration<double, std::milli>(StepEnded - StepStarted).count();
		TelemetryStepMsTotal += StepMs;
		if(StepMs > TelemetryStepMsMax)
			TelemetryStepMsMax = StepMs;
	}
	if(StepsExecuted_ > Limits::MaxInterpreterSteps)
		FailVm("Statement exceeded the VM step limit (safety guard against infinite loops or oversized programs).");
	if(DebugSession_)
		DebugSession_->NotifyCompleted();
	if(Logger_ && StepsExecuted_ > 0) {
		const double MeanStepMs = TelemetryStepMsTotal / static_cast<double>(StepsExecuted_);
		Logger_->Info("VM telemetry steps=" + std::to_string(static_cast<unsigned long long>(StepsExecuted_)) +
		              " mean_ms=" + std::to_string(MeanStepMs) +
		              " max_ms=" + std::to_string(TelemetryStepMsMax) +
		              " failures=" + std::to_string(static_cast<unsigned long long>(TelemetryFailures)));
	}
	if(MutableTimeSqlStats().FastPathFlags == 0)
		(void)TryExecuteDominantAmbBytecode(*this, Code);
	StringOperandPool_ = nullptr;
}

bool BytecodeInterpreter::Step(const Bytecode &Code) {
    if (Ic >= Code.size()) return false;
    const Instruction &inst = Code[Ic];
    if(StepBulk(Code, inst))
        return true;
    if(HandleFusedOpcode(*this, inst)) {
        ++Ic;
        return true;
    }
    switch (inst.Opcode_) {
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
        case Opcode::INT_DIV: {
            int64_t b = static_cast<int64_t>(PopScalarWord("VM INT_DIV"));
            int64_t a = static_cast<int64_t>(PopScalarWord("VM INT_DIV"));
            if(b == 0)
                FailVm("Integer division by zero is not allowed.");
            PushScalarWord(static_cast<uint64_t>(a / b));
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
				StorageLayout CreateStorage = StorageLayout::Row;
				if(inst.Operands.size() > 4) {
					if(auto Sl = std::get_if<int64_t>(&inst.Operands[4]))
						CreateStorage = static_cast<StorageLayout>(*Sl);
				}
				std::string OfTypeName;
				if(inst.Operands.size() > 5) {
					if(auto Ty = std::get_if<std::string>(&inst.Operands[5]))
						OfTypeName = *Ty;
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
                        && (Code[J].Opcode_ == Opcode::PUSH || Code[J].Opcode_ == Opcode::PUSH_POOL)) {
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
                               && (Code[J].Opcode_ == Opcode::PUSH || Code[J].Opcode_ == Opcode::PUSH_POOL)) {
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
                        if (!NN || *NN < 1 || *NN > 32)
                            FailVm("CREATE_TABLE FK bad local column count");
                        ++J;
                        std::vector<std::string> LocalCols;
                        LocalCols.reserve(static_cast<size_t>(*NN));
                        for (int64_t k = 0; k < *NN; ++k) {
                            auto Lcol = StrPushOperand(Code, J, StringOperandPool_);
                            if (!Lcol)
                                FailVm("CREATE_TABLE FK local column");
                            LocalCols.push_back(*Lcol);
                            ++J;
                        }
                        auto Rt = StrPushOperand(Code, J, StringOperandPool_);
                        if (!Rt)
                            FailVm("CREATE_TABLE FK ref table");
                        ++J;
                        auto NRc = IntPushOperand(Code, J);
                        if (!NRc || *NRc != *NN)
                            FailVm("CREATE_TABLE FK ref column count mismatch");
                        ++J;
                        std::vector<std::string> RefCols;
                        RefCols.reserve(static_cast<size_t>(*NRc));
                        for (int64_t k = 0; k < *NRc; ++k) {
                            auto Rc = StrPushOperand(Code, J, StringOperandPool_);
                            if (!Rc)
                                FailVm("CREATE_TABLE FK ref column");
                            RefCols.push_back(*Rc);
                            ++J;
                        }
                        auto OnDel = IntPushOperand(Code, J);
                        if (!OnDel || *OnDel < 0 || *OnDel > 2)
                            FailVm("CREATE_TABLE FK ON DELETE action");
                        ++J;
                        const ReferentialAction Act =
                            static_cast<ReferentialAction>(static_cast<uint8_t>(*OnDel));
                        if (!SkipCreate) {
                            const std::string GroupId = LocalCols.size() > 1
                                ? *tableName + "#FK#" + std::to_string(PostCreateFks.size())
                                : std::string();
                            for (size_t i = 0; i < LocalCols.size(); ++i) {
                                ForeignKey Fk;
                                Fk.ColumnName = LocalCols[i];
                                Fk.ReferencedTable = *Rt;
                                Fk.ReferencedColumn = RefCols[i];
                                Fk.OnDelete = Act;
                                Fk.CompositeGroup = GroupId;
                                PostCreateFks.push_back(std::move(Fk));
                            }
                        }
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
					if(!OfTypeName.empty()) {
						const auto TypeFields = Databases_[0]->ResolveObjectTypeFields(OfTypeName);
						if(!TypeFields.has_value())
							FailVm("CREATE TABLE ... OF references unknown type \"" + OfTypeName + "\".");
						if(!Schema.empty())
							FailVm("Internal: typed table schema must be empty before expansion.");
						for(const auto &Tf : *TypeFields) {
							Database::Column Col;
							Col.Name = Tf.Name;
							Col.DefaultValue = Tf.Type;
							Schema.push_back(std::move(Col));
						}
					}
                    for (Database::Column &Co : Schema) {
                        if(!Co.IsIdentity)
                            continue;
                        if(Co.IdentitySequenceName.empty())
                            Co.IdentitySequenceName = "__astral_id_" + *tableName + "_" + Co.Name;
                    }
                    Databases_[0]->CreateTable(*tableName, Schema, CreateStorage).get();
					if(!OfTypeName.empty())
						Databases_[0]->BindTypedTableToObjectType(*tableName, OfTypeName);
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
		case Opcode::CREATE_TYPE: {
			if(inst.Operands.size() < 2)
				FailVm("CREATE_TYPE expects name and field count");
			const auto *TypeName = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Nf = std::get_if<int64_t>(&inst.Operands[1]);
			if(!TypeName || !Nf || *Nf <= 0)
				FailVm("CREATE_TYPE bad operands");
			if (Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			std::vector<Database::ObjectTypeField> Fields;
			size_t J = Ic + 1;
			for(int64_t K = 0; K < *Nf; ++K) {
				auto FieldName = StrPushOperand(Code, J, StringOperandPool_);
				if(!FieldName)
					FailVm("CREATE_TYPE missing field name");
				++J;
				auto FieldType = StrPushOperand(Code, J, StringOperandPool_);
				if(!FieldType)
					FailVm("CREATE_TYPE missing field type");
				++J;
				Fields.push_back(Database::ObjectTypeField{*FieldName, *FieldType});
			}
			Databases_[0]->CreateObjectType(*TypeName, std::move(Fields));
			Ic = J;
			break;
		}
		case Opcode::DROP_TYPE: {
			if(inst.Operands.empty())
				FailVm("DROP_TYPE expects type name");
			const auto *TypeName = std::get_if<std::string>(&inst.Operands[0]);
			if(!TypeName)
				FailVm("DROP_TYPE bad operand");
			const int64_t IfExists = (inst.Operands.size() > 1 && std::holds_alternative<int64_t>(inst.Operands[1]))
			                             ? std::get<int64_t>(inst.Operands[1])
			                             : 0;
			if (Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			Databases_[0]->DropObjectType(*TypeName, IfExists != 0);
			++Ic;
			break;
		}
        case Opcode::DROP_TABLE: {
            if (inst.Operands.empty()) FailVm("DROP_TABLE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                int64_t DropFlags = 0;
                if (inst.Operands.size() > 1)
                    if (auto F = std::get_if<int64_t>(&inst.Operands[1]))
                        DropFlags = *F;
                const bool IfExists = (DropFlags & 1) != 0;
                const bool Cascade = (DropFlags & 2) != 0;
                if (IfExists) {
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
                Databases_[0]->DropTable(*tableName, Cascade).get();
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
        case Opcode::COMMENT_ON: {
            if(inst.Operands.size() < 4)
                FailVm("COMMENT_ON expects target, table, column, comment");
            const auto *K = std::get_if<int64_t>(&inst.Operands[0]);
            const auto *Tn = std::get_if<std::string>(&inst.Operands[1]);
            const auto *Cn = std::get_if<std::string>(&inst.Operands[2]);
            const auto *Cv = std::get_if<std::string>(&inst.Operands[3]);
            if(!K || !Tn || !Cn || !Cv)
                FailVm("COMMENT_ON operand types");
            if(*K == 0)
                TableComments_[*Tn] = *Cv;
            else
                ColumnComments_[*Tn][*Cn] = *Cv;
            ++Ic;
            break;
        }
        case Opcode::SHOW_TABLES: {
            Database *Db = MutatingDatabase();
            const std::string Out = "__astral_show_tables";
            Database::Schema Sch;
            Database::Column C;
            C.Name = "table_name";
            C.DefaultValue = "TEXT";
            Sch.push_back(std::move(C));
            Database::Table Rows;
            Db->WithExclusiveBytecodeLock([&]() {
                for(const auto &Pr : Db->Tables_) {
                    Database::Item R;
                    R["table_name"] = Pr.first;
                    Rows.push_back(std::move(R));
                }
            });
            Db->ReplaceTableContents(Out, Sch, std::move(Rows));
            PushOwningStringHeap(new std::string(Out));
            ++Ic;
            break;
        }
        case Opcode::DESCRIBE_TABLE: {
            const auto *Tn = inst.Operands.empty() ? nullptr : std::get_if<std::string>(&inst.Operands[0]);
            if(!Tn)
                FailVm("DESCRIBE_TABLE expects table name");
            Database *Db = MutatingDatabase();
            auto SchemaOpt = Db->TableSchemaSnapshot(*Tn);
            if(!SchemaOpt)
                FailVm("DESCRIBE: table not found");
            const std::string Out = "__astral_describe";
            Database::Schema Sch;
            for(const char *Nm : {"column_name", "data_type", "is_nullable", "column_comment"}) {
                Database::Column C;
                C.Name = Nm;
                C.DefaultValue = "TEXT";
                Sch.push_back(std::move(C));
            }
            Database::Table Rows;
            for(const auto &Col : *SchemaOpt) {
                Database::Item R;
                R["column_name"] = Col.Name;
                R["data_type"] = Col.DefaultValue;
                R["is_nullable"] = Col.IsNotNull ? "NO" : "YES";
                auto ItT = ColumnComments_.find(*Tn);
                if(ItT != ColumnComments_.end()) {
                    auto ItC = ItT->second.find(Col.Name);
                    if(ItC != ItT->second.end())
                        R["column_comment"] = ItC->second;
                }
                Rows.push_back(std::move(R));
            }
            Db->ReplaceTableContents(Out, Sch, std::move(Rows));
            PushOwningStringHeap(new std::string(Out));
            ++Ic;
            break;
        }
		case Opcode::CREATE_PROCEDURE: {
			if(inst.Operands.size() < 2)
				FailVm("CREATE_PROCEDURE requires name and body SQL operands");
			const auto *Pn = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Body = std::get_if<std::string>(&inst.Operands[1]);
			if(!Pn || !Body)
				FailVm("CREATE_PROCEDURE expects string name and body operands");
			int64_t IfNotExists = 0;
			int64_t OrReplace = 0;
			if(inst.Operands.size() > 2)
				if(const auto *F = std::get_if<int64_t>(&inst.Operands[2]))
					IfNotExists = *F;
			if(inst.Operands.size() > 3)
				if(const auto *F = std::get_if<int64_t>(&inst.Operands[3]))
					OrReplace = *F;
			std::string SourceDialect;
			std::string ExceptionHandlersJson;
			std::string ControlFlowJson;
			if(inst.Operands.size() > 4)
				if(const auto *D = std::get_if<std::string>(&inst.Operands[4]))
					SourceDialect = *D;
			if(inst.Operands.size() > 5)
				if(const auto *J = std::get_if<std::string>(&inst.Operands[5]))
					ExceptionHandlersJson = *J;
			if(inst.Operands.size() > 6)
				if(const auto *C = std::get_if<std::string>(&inst.Operands[6]))
					ControlFlowJson = *C;
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			Databases_[0]->DefineProcedure(*Pn, *Body, IfNotExists != 0, OrReplace != 0, std::move(SourceDialect),
			                               std::move(ExceptionHandlersJson), std::move(ControlFlowJson));
			++Ic;
			break;
		}
		case Opcode::DROP_PROCEDURE: {
			if(inst.Operands.empty())
				FailVm("DROP_PROCEDURE requires procedure name operand");
			const auto *Pn = std::get_if<std::string>(&inst.Operands[0]);
			if(!Pn)
				FailVm("DROP_PROCEDURE expects string name operand");
			int64_t IfExists = 0;
			if(inst.Operands.size() > 1)
				if(const auto *F = std::get_if<int64_t>(&inst.Operands[1]))
					IfExists = *F;
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			Databases_[0]->DropProcedureDefinition(*Pn, IfExists != 0);
			++Ic;
			break;
		}
		case Opcode::CREATE_TRIGGER: {
			if(inst.Operands.size() < 8)
				FailVm("CREATE_TRIGGER requires operands");
			const auto *Name = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Table = std::get_if<std::string>(&inst.Operands[1]);
			const auto *Timing = std::get_if<int64_t>(&inst.Operands[2]);
			const auto *Event = std::get_if<int64_t>(&inst.Operands[3]);
			const auto *ForEach = std::get_if<int64_t>(&inst.Operands[4]);
			const auto *Kind = std::get_if<std::string>(&inst.Operands[5]);
			const auto *Proc = std::get_if<std::string>(&inst.Operands[6]);
			const auto *Body = std::get_if<std::string>(&inst.Operands[7]);
			if(!Name || !Table || !Timing || !Event || !ForEach || !Kind || !Proc || !Body)
				FailVm("CREATE_TRIGGER operand types");
			int64_t IfNotExists = 0;
			int64_t OrReplace = 0;
			if(inst.Operands.size() > 8)
				if(const auto *F = std::get_if<int64_t>(&inst.Operands[8]))
					IfNotExists = *F;
			if(inst.Operands.size() > 9)
				if(const auto *F = std::get_if<int64_t>(&inst.Operands[9]))
					OrReplace = *F;
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			SQL::StoredTriggerEntry Spec;
			Spec.Name = *Name;
			Spec.TableName = *Table;
			Spec.Timing = static_cast<SQL::TriggerTiming>(*Timing);
			Spec.Event = static_cast<SQL::TriggerEvent>(*Event);
			Spec.ForEachRow = *ForEach != 0;
			Spec.ActionKind = *Kind;
			Spec.ProcedureName = *Proc;
			Spec.BodySql = *Body;
			Spec.Enabled = true;
			Databases_[0]->DefineTrigger(std::move(Spec), IfNotExists != 0, OrReplace != 0);
			++Ic;
			break;
		}
		case Opcode::DROP_TRIGGER: {
			if(inst.Operands.empty())
				FailVm("DROP_TRIGGER requires name");
			const auto *Name = std::get_if<std::string>(&inst.Operands[0]);
			if(!Name)
				FailVm("DROP_TRIGGER expects string name");
			int64_t IfExists = 0;
			if(inst.Operands.size() > 1)
				if(const auto *F = std::get_if<int64_t>(&inst.Operands[1]))
					IfExists = *F;
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			Databases_[0]->DropTriggerDefinition(*Name, IfExists != 0);
			++Ic;
			break;
		}
		case Opcode::ALTER_TRIGGER: {
			if(inst.Operands.size() < 2)
				FailVm("ALTER_TRIGGER requires name and enabled flag");
			const auto *Name = std::get_if<std::string>(&inst.Operands[0]);
			const auto *En = std::get_if<int64_t>(&inst.Operands[1]);
			if(!Name || !En)
				FailVm("ALTER_TRIGGER operand types");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			Databases_[0]->SetTriggerEnabled(*Name, *En != 0);
			++Ic;
			break;
		}
		case Opcode::CALL_PROCEDURE: {
			if(inst.Operands.empty())
				FailVm("CALL_PROCEDURE requires procedure name operand");
			const auto *Pn = std::get_if<std::string>(&inst.Operands[0]);
			if(!Pn)
				FailVm("CALL_PROCEDURE expects string name operand");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			SQL::ProcedureCatalog Catalog;
			const auto CatPath = SQL::DefaultProcedureCatalogPath(Databases_[0]->DbPath_);
			Catalog = SQL::LoadProcedureCatalog(CatPath);
			if(Catalog.CatalogPath.empty())
				Catalog.CatalogPath = CatPath;
			const auto Entry = SQL::FindProcedure(Catalog, *Pn);
			if(!Entry)
				FailVm("Procedure \"" + *Pn + "\" is not registered (CREATE PROCEDURE or --proc-register).");
			const auto Loaded = SQL::LoadProcedureBytecode(*Entry, Catalog);
			const std::vector<std::string> *Pool =
			    Loaded.StringPool.empty() ? nullptr : &Loaded.StringPool;
			RunNestedBytecode(Loaded.Instructions, Pool);
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
                auto *Ta = std::get_if<std::string>(&inst.Operands[1]);
                auto *RnFrom = std::get_if<std::string>(&inst.Operands[2]);
                auto *RnTo = std::get_if<std::string>(&inst.Operands[3]);
                if(!Ta || !RnFrom || !RnTo)
                    FailVm("ALTER TABLE RENAME COLUMN names");
                Db->RenameColumn(*Ta, *RnFrom, *RnTo).get();
            } else if(*AlterKind == 3) {
				if(inst.Operands.size() < 3)
					FailVm("ALTER TABLE SET STORAGE operands");
				auto *Ta = std::get_if<std::string>(&inst.Operands[1]);
				auto *Pol = std::get_if<int64_t>(&inst.Operands[2]);
				if(!Ta || !Pol)
					FailVm("ALTER TABLE SET STORAGE expects table and layout");
				Db->SetTableStoragePolicy(*Ta, static_cast<StorageLayout>(*Pol));
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
			/* Optional RETURNING operands (codegen appends after the base INSERT header): */
			/* retTable (string), retReset (int64 0/1), retNRet (int64, -1=*, else explicit count), retCols... */
			bool DoReturning = false;
			std::string RetTable;
			int64_t RetReset = 0;
			int64_t RetNRet = 0;
			std::vector<std::string> RetCols;
			if(inst.Operands.size() > 2) {
				if(inst.Operands.size() < 5)
					FailVm("INSERT returning operands truncated");
				auto *RT = std::get_if<std::string>(&inst.Operands[2]);
				auto *RR = std::get_if<int64_t>(&inst.Operands[3]);
				auto *RN = std::get_if<int64_t>(&inst.Operands[4]);
				if(!RT || !RR || !RN)
					FailVm("INSERT returning operand types");
				if(*RR != 0 && *RR != 1)
					FailVm("INSERT returning retReset must be 0/1");
				if(*RN < -1)
					FailVm("INSERT returning retNRet out of range");
				RetTable = *RT;
				RetReset = *RR;
				RetNRet = *RN;
				const size_t Need = RetNRet < 0 ? 5 : (5 + static_cast<size_t>(RetNRet));
				if(inst.Operands.size() != Need)
					FailVm("INSERT returning operand count mismatch");
				if(RetNRet >= 0) {
					RetCols.reserve(static_cast<size_t>(RetNRet));
					for(int64_t i = 0; i < RetNRet; ++i) {
						auto *C = std::get_if<std::string>(&inst.Operands[5 + static_cast<size_t>(i)]);
						if(!C)
							FailVm("INSERT returning ret column expects string");
						RetCols.push_back(*C);
					}
				}
				DoReturning = true;
			}
            const int64_t K64 = *KPtr;
            if (K64 < 0 || K64 > 100000) FailVm("INSERT value count out of range");
            const size_t K = static_cast<size_t>(K64);
            Database *Db = MutatingDatabase();

            auto PopBorrowedStr = [&]() -> std::string {
                return PopOwnedStringMoved("INSERT");
            };

            std::vector<std::string> Values(K);
            for (size_t i = K; i-- > 0;)
                Values[i] = PopBorrowedStr();

			std::string TableName;
            Database::Item Row;
            if (*ExplicitPtr) {
                std::vector<std::string> ColKeys(K);
                for (size_t i = K; i-- > 0;)
                    ColKeys[i] = PopBorrowedStr();
                TableName = PopBorrowedStr();
                for (size_t i = 0; i < K; ++i)
                    Row[ColKeys[i]] = Values[i];
            } else {
                TableName = PopBorrowedStr();
                const auto Snap = DatabaseVmAssumeDbMutexHeld() ? Db->TableSchemaAssumeDbMutexHeld(TableName)
                                                                : Db->TableSchemaSnapshot(TableName);
                if (!Snap || Snap->size() != K)
                    FailVm("INSERT implicit columns require matching table schema");
                for (size_t i = 0; i < K; ++i)
                    Row[(*Snap)[i].Name] = Values[i];
            }

			/* RETURNING path materializes the affected row into __astral_returning. */
			if(DoReturning) {
				constexpr const char *kNextValPrefix = "__astral_nextval__:";
				auto TargetSchemaOpt = Db->TableSchemaSnapshot(TableName);
				if(!TargetSchemaOpt)
					FailVm("INSERT RETURNING: missing table schema");
				const auto &TargetSchema = *TargetSchemaOpt;

				// Resolve NEXTVAL placeholders and generated-by-default identity columns so Row matches inserted values.
				for(auto &[Col, Val] : Row) {
					if(Val.rfind(kNextValPrefix, 0) == 0) {
						const std::string Seq = Val.substr(std::string_view(kNextValPrefix).size());
						Val = Db->NextSequenceValue(Seq);
					}
					(void)Col;
				}
				for(const auto &Co : TargetSchema) {
					if(!Co.IsIdentity || Co.IdentitySequenceName.empty())
						continue;
					auto It = Row.find(Co.Name);
					const bool Missing = It == Row.end() || It->second.empty() || It->second == "NULL";
					if(!Missing)
						continue;
					if(Co.IdentityAlways)
						FailVm("INSERT RETURNING does not support GENERATED ALWAYS identity columns without an explicit value");
					Row[Co.Name] = Db->NextSequenceValue(Co.IdentitySequenceName);
				}

				std::vector<std::string> OutCols;
				if(RetNRet < 0) {
					OutCols.reserve(TargetSchema.size());
					for(const auto &Co : TargetSchema)
						OutCols.push_back(Co.Name);
				} else {
					OutCols = RetCols;
					for(const std::string &C : OutCols) {
						bool Found = false;
						for(const auto &Co : TargetSchema) {
							if(Co.Name == C) {
								Found = true;
								break;
							}
						}
						if(!Found)
							FailVm("INSERT RETURNING: unknown column \"" + C + "\"");
					}
				}

				Database::Schema RetSchema;
				RetSchema.reserve(OutCols.size());
				for(const std::string &C : OutCols) {
					Database::Column RC;
					RC.Name = C;
					RC.DefaultValue = "TEXT";
					RetSchema.push_back(std::move(RC));
				}

				if(RetReset != 0 || !Db->TableSchemaSnapshot(RetTable).has_value())
					Db->ReplaceTableContents(RetTable, RetSchema, {});

				Db->Insert(TableName, Row).get();

				Database::Item RetRow;
				for(const std::string &C : OutCols) {
					auto It = Row.find(C);
					RetRow[C] = It == Row.end() ? std::string() : It->second;
				}
				Db->Insert(RetTable, RetRow).get();
			} else {
				Db->Insert(TableName, Row).get();
			}

            ++Ic;
            break;
        }
        case Opcode::REGISTER_DATASET: {
            if(inst.Operands.size() != 6)
                FailVm("REGISTER_DATASET expects six operands");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            auto *Kind = std::get_if<int64_t>(&inst.Operands[1]);
            auto *Src = std::get_if<std::string>(&inst.Operands[2]);
            auto *Cnt = std::get_if<int64_t>(&inst.Operands[3]);
            auto *Start = std::get_if<int64_t>(&inst.Operands[4]);
            auto *Step = std::get_if<int64_t>(&inst.Operands[5]);
            if(!Name || !Kind || !Src || !Cnt || !Start || !Step)
                FailVm("REGISTER_DATASET operand types");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            DatasetEntry Ent;
            Ent.Kind = *Kind == 0 ? DatasetKind::TableRef : DatasetKind::BulkFixture;
            Ent.SourceTable = *Src;
            Ent.BulkCount = *Cnt;
            Ent.BulkStart = *Start;
            Ent.BulkStep = *Step;
            Databases_[0]->RegisterDataset(*Name, std::move(Ent));
            ++Ic;
            break;
        }
        case Opcode::LOAD_DATASET: {
            if(inst.Operands.size() < 2 || inst.Operands.size() > 3)
                FailVm("LOAD_DATASET expects dataset, target table, optional version");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            auto *Tgt = std::get_if<std::string>(&inst.Operands[1]);
            int64_t Ver = 0;
            if(inst.Operands.size() == 3) {
                auto *V = std::get_if<int64_t>(&inst.Operands[2]);
                if(!V)
                    FailVm("LOAD_DATASET version must be int64");
                Ver = *V;
            }
            if(!Name || !Tgt)
                FailVm("LOAD_DATASET operand types");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->LoadDatasetInto(*Name, *Tgt, Ver);
            ++Ic;
            break;
        }
        case Opcode::VACUUM: {
            if(inst.Operands.size() != 1)
                FailVm("VACUUM expects optional table name operand");
            auto *Tbl = std::get_if<std::string>(&inst.Operands[0]);
            if(!Tbl)
                FailVm("VACUUM operand types");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->Vacuum(*Tbl);
            ++Ic;
            break;
        }
        case Opcode::REPACK_CONCURRENTLY: {
            if(inst.Operands.size() != 1)
                FailVm("REPACK_CONCURRENTLY expects table name");
            auto *Tbl = std::get_if<std::string>(&inst.Operands[0]);
            if(!Tbl || Tbl->empty())
                FailVm("REPACK_CONCURRENTLY table name required");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->RepackTableConcurrently(*Tbl);
            ++Ic;
            break;
        }
        case Opcode::DROP_DATASET: {
            if(inst.Operands.size() != 1)
                FailVm("DROP_DATASET expects dataset name");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            if(!Name)
                FailVm("DROP_DATASET operand types");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->DropDataset(*Name);
            ++Ic;
            break;
        }
        case Opcode::REGISTER_EMBEDDING: {
            if(inst.Operands.size() != 4)
                FailVm("REGISTER_EMBEDDING expects four operands");
            const auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            const auto *Table = std::get_if<std::string>(&inst.Operands[1]);
            const auto *Tok = std::get_if<std::string>(&inst.Operands[2]);
            const auto *Vec = std::get_if<std::string>(&inst.Operands[3]);
            if(!Name || !Table || !Tok || !Vec)
                FailVm("REGISTER_EMBEDDING operand types");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->RegisterEmbedding(*Name, *Table, *Tok, *Vec);
            ++Ic;
            break;
        }
        case Opcode::DROP_EMBEDDING: {
            if(inst.Operands.size() != 1)
                FailVm("DROP_EMBEDDING expects embedding name");
            const auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            if(!Name)
                FailVm("DROP_EMBEDDING operand types");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->DropEmbedding(*Name);
            ++Ic;
            break;
        }
		case Opcode::GRAPH_REGISTER: {
			if(inst.Operands.size() != 9)
				FailVm("GRAPH_REGISTER expects nine operands");
			const auto *Gn = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Vt = std::get_if<std::string>(&inst.Operands[1]);
			const auto *Vid = std::get_if<std::string>(&inst.Operands[2]);
			const auto *Et = std::get_if<std::string>(&inst.Operands[3]);
			const auto *Src = std::get_if<std::string>(&inst.Operands[4]);
			const auto *Dst = std::get_if<std::string>(&inst.Operands[5]);
			const auto *Lbl = std::get_if<std::string>(&inst.Operands[6]);
			const auto *Wt = std::get_if<std::string>(&inst.Operands[7]);
			const auto *Und = std::get_if<int64_t>(&inst.Operands[8]);
			if(!Gn || !Vt || !Vid || !Et || !Src || !Dst || !Lbl || !Wt || !Und)
				FailVm("GRAPH_REGISTER operand types");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			GraphSpec Spec;
			Spec.Name = *Gn;
			Spec.VertexTable = *Vt;
			Spec.VertexIdCol = *Vid;
			Spec.EdgeTable = *Et;
			Spec.EdgeSrcCol = *Src;
			Spec.EdgeDstCol = *Dst;
			Spec.EdgeLabelCol = *Lbl;
			Spec.EdgeWeightCol = *Wt;
			Spec.Undirected = *Und != 0;
			Databases_[0]->RegisterGraph(std::move(Spec));
			++Ic;
			break;
		}
		case Opcode::GRAPH_REGISTER_PROJECTION: {
			if(inst.Operands.size() != 3)
				FailVm("GRAPH_REGISTER_PROJECTION expects three operands");
			const auto *Pn = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Bn = std::get_if<std::string>(&inst.Operands[1]);
			const auto *Fl = std::get_if<std::string>(&inst.Operands[2]);
			if(!Pn || !Bn || !Fl)
				FailVm("GRAPH_REGISTER_PROJECTION operand types");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			GraphProjectionRequest Req;
			Req.ProjectionName = *Pn;
			Req.BaseGraphName = *Bn;
			Req.EdgeLabelFilter = *Fl;
			Databases_[0]->RegisterGraphProjection(Req);
			++Ic;
			break;
		}
		case Opcode::GRAPH_DROP: {
			if(inst.Operands.size() != 1)
				FailVm("GRAPH_DROP expects graph name");
			const auto *Gn = std::get_if<std::string>(&inst.Operands[0]);
			if(!Gn)
				FailVm("GRAPH_DROP operand types");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			Databases_[0]->DropGraph(*Gn);
			++Ic;
			break;
		}
		case Opcode::GRAPH_TRAVERSE: {
			if(inst.Operands.size() != 5)
				FailVm("GRAPH_TRAVERSE expects five operands");
			const auto *Gn = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Start = std::get_if<std::string>(&inst.Operands[1]);
			const auto *Depth = std::get_if<int64_t>(&inst.Operands[2]);
			const auto *Mode = std::get_if<int64_t>(&inst.Operands[3]);
			const auto *Result = std::get_if<std::string>(&inst.Operands[4]);
			if(!Gn || !Start || !Depth || !Mode || !Result)
				FailVm("GRAPH_TRAVERSE operand types");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			GraphTraverseRequest Req;
			Req.GraphName = *Gn;
			Req.StartVertexId = *Start;
			Req.MaxDepth = *Depth;
			Req.Mode = *Mode != 0 ? GraphTraverseMode::Dfs : GraphTraverseMode::Bfs;
			Req.ResultTable = *Result;
			Databases_[0]->GraphTraverse(Req);
			++Ic;
			break;
		}
		case Opcode::GRAPH_MATCH: {
			if(inst.Operands.size() != 7)
				FailVm("GRAPH_MATCH expects seven operands");
			const auto *Gn = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Filter = std::get_if<std::string>(&inst.Operands[1]);
			const auto *Result = std::get_if<std::string>(&inst.Operands[2]);
			const auto *MinH = std::get_if<int64_t>(&inst.Operands[3]);
			const auto *MaxH = std::get_if<int64_t>(&inst.Operands[4]);
			const auto *Anchor = std::get_if<std::string>(&inst.Operands[5]);
			const auto *Rev = std::get_if<int64_t>(&inst.Operands[6]);
			if(!Gn || !Filter || !Result || !MinH || !MaxH || !Anchor || !Rev)
				FailVm("GRAPH_MATCH operand types");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			GraphMatchRequest Req;
			Req.GraphName = *Gn;
			Req.EdgeLabelFilter = *Filter;
			Req.ResultTable = *Result;
			Req.MinHops = *MinH;
			Req.MaxHops = *MaxH;
			Req.AnchorVertexId = *Anchor;
			Req.Reverse = *Rev != 0;
			Databases_[0]->GraphMatch(Req);
			++Ic;
			break;
		}
		case Opcode::GRAPH_SHORTEST_PATH: {
			if(inst.Operands.size() != 5)
				FailVm("GRAPH_SHORTEST_PATH expects five operands");
			const auto *Gn = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Fr = std::get_if<std::string>(&inst.Operands[1]);
			const auto *To = std::get_if<std::string>(&inst.Operands[2]);
			const auto *Wt = std::get_if<int64_t>(&inst.Operands[3]);
			const auto *Result = std::get_if<std::string>(&inst.Operands[4]);
			if(!Gn || !Fr || !To || !Wt || !Result)
				FailVm("GRAPH_SHORTEST_PATH operand types");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			GraphShortestPathRequest Req;
			Req.GraphName = *Gn;
			Req.FromVertexId = *Fr;
			Req.ToVertexId = *To;
			Req.Weighted = *Wt != 0;
			Req.ResultTable = *Result;
			Databases_[0]->GraphShortestPath(Req);
			++Ic;
			break;
		}
		case Opcode::GRAPH_PAGERANK: {
			if(inst.Operands.size() != 4)
				FailVm("GRAPH_PAGERANK expects four operands");
			const auto *Gn = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Damp = std::get_if<int64_t>(&inst.Operands[1]);
			const auto *Iter = std::get_if<int64_t>(&inst.Operands[2]);
			const auto *Result = std::get_if<std::string>(&inst.Operands[3]);
			if(!Gn || !Damp || !Iter || !Result)
				FailVm("GRAPH_PAGERANK operand types");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			GraphPageRankRequest Req;
			Req.GraphName = *Gn;
			Req.DampingMillis = *Damp;
			Req.Iterations = *Iter;
			Req.ResultTable = *Result;
			Databases_[0]->GraphPageRank(Req);
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
            if(inst.Operands.size() < 4 + NConflict + 2)
                FailVm("UPSERT missing replace-mode / assignment count");
            const size_t ReplaceModeIdx = 4 + NConflict;
            const size_t SetCountIdx = ReplaceModeIdx + 1;
            auto *ReplaceModePtr = std::get_if<int64_t>(&inst.Operands[ReplaceModeIdx]);
            auto *NSetPtr = std::get_if<int64_t>(&inst.Operands[SetCountIdx]);
            if(!ReplaceModePtr || !NSetPtr || *NSetPtr < 0)
                FailVm("UPSERT SET count invalid");
            const int64_t ReplaceMode = *ReplaceModePtr;
            const size_t NSet = static_cast<size_t>(*NSetPtr);
			const size_t PairsBase = SetCountIdx + 1;
			const size_t ExpectedEnd = PairsBase + 2 * NSet;
			if(inst.Operands.size() < ExpectedEnd)
				FailVm("UPSERT operand tail size truncated");

			/* Optional RETURNING tail operands appended after SET assignments. */
			bool DoReturning = false;
			std::string RetTable;
			int64_t RetReset = 0;
			int64_t RetNRet = 0;
			std::vector<std::string> RetCols;
			if(inst.Operands.size() > ExpectedEnd) {
				const size_t RetIdx = ExpectedEnd;
				if(inst.Operands.size() < RetIdx + 3)
					FailVm("UPSERT returning tail operands truncated");
				auto *RT = std::get_if<std::string>(&inst.Operands[RetIdx]);
				auto *RR = std::get_if<int64_t>(&inst.Operands[RetIdx + 1]);
				auto *RN = std::get_if<int64_t>(&inst.Operands[RetIdx + 2]);
				if(!RT || !RR || !RN)
					FailVm("UPSERT returning operand types");
				if(*RR != 0 && *RR != 1)
					FailVm("UPSERT returning retReset must be 0/1");
				if(*RN < -1)
					FailVm("UPSERT returning retNRet out of range");
				RetTable = *RT;
				RetReset = *RR;
				RetNRet = *RN;
				const size_t Need = RetNRet < 0 ? (RetIdx + 3) : (RetIdx + 3 + static_cast<size_t>(RetNRet));
				if(inst.Operands.size() != Need)
					FailVm("UPSERT returning operand count mismatch");
				if(RetNRet >= 0) {
					RetCols.reserve(static_cast<size_t>(RetNRet));
					for(int64_t i = 0; i < RetNRet; ++i) {
						auto *C = std::get_if<std::string>(&inst.Operands[RetIdx + 3 + static_cast<size_t>(i)]);
						if(!C)
							FailVm("UPSERT returning ret column expects string");
						RetCols.push_back(*C);
					}
				}
				DoReturning = true;
			}
            std::vector<std::string> ConflictCols;
            ConflictCols.reserve(NConflict);
            for(size_t i = 0; i < NConflict; ++i) {
                if(auto *S = std::get_if<std::string>(&inst.Operands[4 + i]))
                    ConflictCols.push_back(*S);
                else
                    FailVm("UPSERT conflict column name must be string");
            }
            std::vector<std::pair<std::string, std::string>> UpdateAssignments;
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

			bool SkipReturningRow = false;
			std::vector<std::string> OutCols;
			Database::Schema RetSchema;
			if(DoReturning) {
				constexpr const char *kNextValPrefix = "__astral_nextval__:";
				auto TargetSchemaOpt = Db->TableSchemaSnapshot(TableName);
				if(!TargetSchemaOpt)
					FailVm("UPSERT RETURNING: missing table schema");
				const auto &TargetSchema = *TargetSchemaOpt;

				if(RetNRet < 0) {
					OutCols.reserve(TargetSchema.size());
					for(const auto &Co : TargetSchema)
						OutCols.push_back(Co.Name);
				} else {
					OutCols = RetCols;
					for(const std::string &C : OutCols) {
						bool Found = false;
						for(const auto &Co : TargetSchema) {
							if(Co.Name == C) {
								Found = true;
								break;
							}
						}
						if(!Found)
							FailVm("UPSERT RETURNING: unknown column \"" + C + "\"");
					}
				}

				RetSchema.reserve(OutCols.size());
				for(const std::string &C : OutCols) {
					Database::Column RC;
					RC.Name = C;
					RC.DefaultValue = "TEXT";
					RetSchema.push_back(std::move(RC));
				}

				if(RetReset != 0 || !Db->TableSchemaSnapshot(RetTable).has_value())
					Db->ReplaceTableContents(RetTable, RetSchema, {});

				// Resolve NEXTVAL placeholders so Row matches what will be inserted/updated.
				for(auto &[Col, Val] : Row) {
					if(Val.rfind(kNextValPrefix, 0) == 0) {
						const std::string Seq = Val.substr(std::string_view(kNextValPrefix).size());
						Val = Db->NextSequenceValue(Seq);
					}
					(void)Col;
				}

				// Fill generated-by-default identity columns (needed because Db->Upsert doesn't auto-fill identities).
				for(const auto &Co : TargetSchema) {
					if(!Co.IsIdentity || Co.IdentitySequenceName.empty())
						continue;
					auto It = Row.find(Co.Name);
					const bool Missing = It == Row.end() || It->second.empty() || It->second == "NULL";
					if(!Missing)
						continue;
					if(Co.IdentityAlways)
						FailVm("UPSERT RETURNING does not support GENERATED ALWAYS identity columns without an explicit value");
					Row[Co.Name] = Db->NextSequenceValue(Co.IdentitySequenceName);
				}

				// For ON CONFLICT DO NOTHING, we can at least avoid returning a pre-existing row by checking existence before the Upsert.
				if(*DoNothingPtr != 0) {
					std::vector<std::string> Keys = ConflictCols;
					if(Keys.empty()) {
						for(const auto &Co : TargetSchema)
							if(Co.IsPrimaryKey)
								Keys.push_back(Co.Name);
					}
					if(Keys.empty())
						FailVm("UPSERT RETURNING: ON CONFLICT DO NOTHING requires primary key to suppress returns");

					auto Existing = Db->Select(TableName, [&](const Database::Item &R) {
						for(const std::string &K : Keys) {
							auto ItE = R.find(K);
							auto ItI = Row.find(K);
							if(ItE == R.end() || ItI == Row.end() || ItE->second != ItI->second)
								return false;
						}
						return true;
					}).get();
					if(!Existing.empty())
						SkipReturningRow = true;
				}
			}
            if(*DoNothingPtr == 0) {
                if(ReplaceMode == 2 && NSet == 0) {
                    auto Snap = Db->TableSchemaSnapshot(TableName);
                    if(!Snap)
                        FailVm("REPLACE INTO: missing table schema");
                    for(const auto &Co : *Snap) {
                        if(Row.find(Co.Name) == Row.end())
                            continue;
                        UpdateAssignments.emplace_back(Co.Name,
                                                      std::string("E") + Co.Name + "|");
                    }
                } else if(NSet > 0) {
                    UpdateAssignments.reserve(NSet);
                    for(size_t s = 0; s < NSet; ++s) {
                        auto *ColN = std::get_if<std::string>(&inst.Operands[PairsBase + s * 2]);
                        auto *Blob = std::get_if<std::string>(&inst.Operands[PairsBase + s * 2 + 1]);
                        if(!ColN || !Blob)
                            FailVm("UPSERT DO UPDATE SET expects column/expression");
                        UpdateAssignments.emplace_back(*ColN, *Blob);
                    }
                }
            }
            Db->Upsert(TableName, Row, ConflictCols, *DoNothingPtr != 0, UpdateAssignments,
                       ReplaceMode == 1 || ReplaceMode == 2)
                .get();

			if(DoReturning && !SkipReturningRow) {
				std::vector<std::string> Keys = ConflictCols;
				if(Keys.empty()) {
					auto TargetSchemaOpt = Db->TableSchemaSnapshot(TableName);
					if(!TargetSchemaOpt)
						FailVm("UPSERT RETURNING: missing table schema for key selection");
					for(const auto &Co : *TargetSchemaOpt)
						if(Co.IsPrimaryKey)
							Keys.push_back(Co.Name);
				}
				auto Matches = Db->Select(TableName, [&](const Database::Item &R) {
					for(const std::string &K : Keys) {
						auto ItE = R.find(K);
						auto ItI = Row.find(K);
						if(ItE == R.end() || ItI == Row.end() || ItE->second != ItI->second)
							return false;
					}
					return true;
				}).get();
				for(const auto &M : Matches) {
					Database::Item RetRow;
					for(const std::string &C : OutCols) {
						auto It = M.find(C);
						RetRow[C] = It == M.end() ? std::string() : It->second;
					}
					Db->Insert(RetTable, RetRow).get();
				}
			}
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
                int64_t TotalCols = *Count;
                const bool Dynamic = inst.Operands.size() >= 2 && std::get_if<int64_t>(&inst.Operands[1]) &&
                                     *std::get_if<int64_t>(&inst.Operands[1]) != 0;
                if(Dynamic)
                    TotalCols += static_cast<int64_t>(ColumnsExpandExtra_);
                if(TotalCols < 0 || TotalCols > 256)
                    FailVm("SELECT finalize: invalid column count");
                const size_t Need = static_cast<size_t>(TotalCols) + 1;
                if(StackSlots_.size() < Need)
                    FailVm("SELECT finalize: stack underflow");
                (void)PopOwnedStringMoved("SELECT finalize table");
                for(int64_t I = 0; I < TotalCols; ++I)
                    (void)PopOwnedStringMoved("SELECT finalize column");
                ColumnsExpandExtra_ = 0;
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
        case Opcode::STORAGE_HINT: {
			if(inst.Operands.empty())
				FailVm("STORAGE_HINT requires layout operand");
			if(auto *Layout = std::get_if<int64_t>(&inst.Operands[0]))
				SessionStorageHint_ = static_cast<StorageLayout>(*Layout);
			else
				FailVm("STORAGE_HINT layout must be int64");
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
                if (StackSlots_.size() < 3)
                    FailVm("ORDER_BY requires ascending flag, nulls-first flag, and table name on stack");
                const bool NullsFirst = PopScalarWord("ORDER_BY nulls") != 0;
                const bool Ascending = PopScalarWord("ORDER_BY ascending") != 0;
                std::string TableName = PopOwnedStringMoved("ORDER_BY table");
                
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(DatabasePath_));
                }
                Databases_[0]->WithExclusiveBytecodeLock([&]() {
                    auto &Table = Databases_[0]->Tables_[TableName].RowStore;
                    std::sort(Table.begin(), Table.end(),
                        [Column, Ascending, NullsFirst](const Database::Item& a, const Database::Item& b) {
                            auto itA = a.find(*Column);
                            auto itB = b.find(*Column);
                            const bool MissingA = itA == a.end();
                            const bool MissingB = itB == b.end();
                            if(MissingA && MissingB) return false;
                            if(MissingA) return NullsFirst;
                            if(MissingB) return !NullsFirst;
                            if(itA->second == itB->second) return false;
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
				HybridTableSlot &Slot = DbGrp->Tables_[TableName];
				(void)Slot.RowsForRead(SessionStorageHint_, true);
				auto &Tbl = Slot.RowStore;
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
                Superfetch::RowScanSession GbScan;
                Superfetch::BeginRowScan(Tbl, GbScan);
                for(std::size_t Tri = 0; Tri < Tbl.size(); ++Tri) {
                    Superfetch::AdvanceRowScan(Tbl, Tri, GbScan);
                    const auto &Row = Tbl[Tri];
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
                Superfetch::RowScanSession GbScan;
                Superfetch::BeginRowScan(Tbl, GbScan);
                for(std::size_t Tri = 0; Tri < Tbl.size(); ++Tri) {
                    Superfetch::AdvanceRowScan(Tbl, Tri, GbScan);
                    const auto &Row = Tbl[Tri];
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
                Superfetch::RowScanSession GbScan;
                Superfetch::BeginRowScan(Tbl, GbScan);
                for(std::size_t Tri = 0; Tri < Tbl.size(); ++Tri) {
                    Superfetch::AdvanceRowScan(Tbl, Tri, GbScan);
                    const auto &Row = Tbl[Tri];
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
                Superfetch::RowScanSession GbScan;
                Superfetch::BeginRowScan(Tbl, GbScan);
                for(std::size_t Tri = 0; Tri < Tbl.size(); ++Tri) {
                    Superfetch::AdvanceRowScan(Tbl, Tri, GbScan);
                    const auto &Row = Tbl[Tri];
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
        case Opcode::CONNECT_BY_EXPAND: {
            if(inst.Operands.size() < 6)
                FailVm("CONNECT_BY_EXPAND expects table, parent col, child col, prior flag, start blob, nocycle");
            const auto *Tab = std::get_if<std::string>(&inst.Operands[0]);
            const auto *ParentCol = std::get_if<std::string>(&inst.Operands[1]);
            const auto *ChildCol = std::get_if<std::string>(&inst.Operands[2]);
            const auto *PriorPtr = std::get_if<int64_t>(&inst.Operands[3]);
            const auto *StartBlob = std::get_if<std::string>(&inst.Operands[4]);
            const auto *NoCyclePtr = std::get_if<int64_t>(&inst.Operands[5]);
            if(!Tab || Tab->empty() || !ParentCol || ParentCol->empty() || !ChildCol || ChildCol->empty() ||
               !PriorPtr || !StartBlob || !NoCyclePtr)
                FailVm("CONNECT_BY_EXPAND: bad operands");
            (void)PriorPtr;
            const bool NoCycle = *NoCyclePtr != 0;
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
            Db->WithExclusiveBytecodeLock([&]() {
                auto Tit = Db->Tables_.find(*Tab);
                if(Tit == Db->Tables_.end())
                    FailVm("CONNECT_BY_EXPAND: table missing");
                const Database::Table AllRows = Tit->second.RowStore;
                std::vector<Database::Item> Roots;
                if(StartBlob->empty()) {
                    Roots.assign(AllRows.begin(), AllRows.end());
                } else {
                    std::vector<std::vector<RowTriple>> Branches;
                    if(!UnpackDnfBlobToBranches(*StartBlob, Branches))
                        FailVm("CONNECT_BY_EXPAND: corrupt START WITH blob");
                    for(const Database::Item &Row : AllRows) {
                        if(MatchWhereDnf(Db, *Tab, Row, Branches))
                            Roots.push_back(Row);
                    }
                }
                std::vector<Database::Item> Out;
                Out.reserve(AllRows.size());
                const auto LinkChild = [&](const Database::Item &Parent, const Database::Item &Child) -> bool {
                    auto Pp = Parent.find(*ParentCol);
                    auto Cc = Child.find(*ChildCol);
                    const std::string Pv = Pp == Parent.end() ? std::string() : Pp->second;
                    const std::string Cv = Cc == Child.end() ? std::string() : Cc->second;
                    return !Pv.empty() && Pv == Cv;
                };
                std::function<void(const Database::Item &, std::vector<std::string> &)> Visit;
                Visit = [&](const Database::Item &Parent, std::vector<std::string> &AncestorKeys) {
                    if(Out.size() >= Limits::MaxCteRecursionDepth)
                        return;
                    Out.push_back(Parent);
                    auto Pk = Parent.find(*ParentCol);
                    const std::string Key = Pk == Parent.end() ? std::string() : Pk->second;
                    if(!Key.empty())
                        AncestorKeys.push_back(Key);
                    for(const Database::Item &Cand : AllRows) {
                        if(!LinkChild(Parent, Cand))
                            continue;
                        if(NoCycle) {
                            auto Ck = Cand.find(*ChildCol);
                            const std::string Ckey = Ck == Cand.end() ? std::string() : Ck->second;
                            bool OnPath = false;
                            for(const std::string &A : AncestorKeys) {
                                if(A == Ckey) {
                                    OnPath = true;
                                    break;
                                }
                            }
                            if(OnPath)
                                continue;
                        }
                        Visit(Cand, AncestorKeys);
                    }
                    if(!Key.empty())
                        AncestorKeys.pop_back();
                };
                for(const Database::Item &Root : Roots) {
                    std::vector<std::string> Anc;
                    Visit(Root, Anc);
                }
                Tit->second.RowStore = std::move(Out);
                Tit->second.RecordWrite();
                Tit->second.SyncColumnarAfterRowMutation();
            });
            ++Ic;
            break;
        }
        case Opcode::RECURSIVE_CTE_FIXPOINT: {
            if(inst.Operands.size() < 4)
                FailVm("RECURSIVE_CTE_FIXPOINT expects work, delta, max_iterations, loop_start");
            const auto *WorkNm = std::get_if<std::string>(&inst.Operands[0]);
            const auto *DeltaNm = std::get_if<std::string>(&inst.Operands[1]);
            const auto *MaxIterOp = std::get_if<int64_t>(&inst.Operands[2]);
            const auto *LoopStartOp = std::get_if<int64_t>(&inst.Operands[3]);
            if(!WorkNm || WorkNm->empty() || !DeltaNm || DeltaNm->empty() || !MaxIterOp || !LoopStartOp)
                FailVm("RECURSIVE_CTE_FIXPOINT: bad operands");
            if(*MaxIterOp < 0)
                FailVm("RECURSIVE_CTE_FIXPOINT: max_iterations must be non-negative");
            const size_t FixIp = Ic;
            const size_t LoopStart = static_cast<size_t>(*LoopStartOp);
            if(LoopStart > FixIp)
                FailVm("RECURSIVE_CTE_FIXPOINT: loop_start past fixpoint instruction");
            EnsurePrimaryDatabaseOpened();
            Database *Db = Databases_[0].get();
            int64_t Remaining = *MaxIterOp;
            auto MergeDeltaIntoWork = [&]() -> bool {
                bool Grew = false;
                Db->WithExclusiveBytecodeLock([&]() {
                    auto WIt = Db->Tables_.find(*WorkNm);
                    auto DIt = Db->Tables_.find(*DeltaNm);
                    if(WIt == Db->Tables_.end() || DIt == Db->Tables_.end())
                        FailVm("RECURSIVE_CTE_FIXPOINT: work or delta table missing");
                    std::unordered_set<std::string> Seen;
                    for(const Database::Item &R : WIt->second.RowStore)
                        Seen.insert(RowSignatureCanon(R));
                    for(const Database::Item &R : DIt->second.RowStore) {
                        if(Seen.insert(RowSignatureCanon(R)).second) {
                            WIt->second.RowStore.push_back(R);
                            Grew = true;
                        }
                    }
                    DIt->second.RowStore.clear();
                });
                return Grew;
            };
            if(!MergeDeltaIntoWork())
                Remaining = 0;
            while(Remaining > 0) {
                CleanupStack();
                Ic = LoopStart;
                while(Ic < FixIp) {
                    if(++StepsExecuted_ > Limits::MaxInterpreterSteps)
                        FailVm("Statement exceeded the VM step limit (recursive CTE fixpoint loop).");
                    if(!Step(Code))
                        FailVm("HALT inside recursive CTE fixpoint loop");
                }
                if(!MergeDeltaIntoWork())
                    break;
                --Remaining;
            }
            Ic = FixIp + 1;
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
            Database *Db = Databases_[0].get();
            if(*Src == "__astral_information_schema_tables") {
                Database::Schema Sch;
                for(const char *Nm : {"table_schema", "table_name"}) {
                    Database::Column C;
                    C.Name = Nm;
                    C.DefaultValue = "TEXT";
                    Sch.push_back(std::move(C));
                }
                Database::Table Rows;
                Db->WithExclusiveBytecodeLock([&]() {
                    for(const auto &Pr : Db->Tables_) {
                        Database::Item R;
                        R["table_schema"] = "public";
                        R["table_name"] = Pr.first;
                        Rows.push_back(std::move(R));
                    }
                });
                Db->ReplaceTableContents(*Dest, Sch, std::move(Rows));
            } else if(*Src == "__astral_information_schema_columns") {
                Database::Schema Sch;
                for(const char *Nm : {"table_schema", "table_name", "column_name", "data_type"}) {
                    Database::Column C;
                    C.Name = Nm;
                    C.DefaultValue = "TEXT";
                    Sch.push_back(std::move(C));
                }
                Database::Table Rows;
                Db->WithExclusiveBytecodeLock([&]() {
                    for(const auto &Pr : Db->Tables_) {
                        const auto Snap = Db->TableSchemaAssumeDbMutexHeld(Pr.first);
                        if(!Snap)
                            continue;
                        for(const auto &Col : *Snap) {
                            Database::Item R;
                            R["table_schema"] = "public";
                            R["table_name"] = Pr.first;
                            R["column_name"] = Col.Name;
                            R["data_type"] = Col.DefaultValue;
                            Rows.push_back(std::move(R));
                        }
                    }
                });
                Db->ReplaceTableContents(*Dest, Sch, std::move(Rows));
            } else {
                Db->CloneTable(*Dest, *Src);
            }
            ++Ic;
            break;
        }
		case Opcode::FILTER_AS_OF: {
			if(inst.Operands.size() < 2)
				FailVm("FILTER_AS_OF expects table name and timestamp");
			const auto *Tab = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Ts = std::get_if<std::string>(&inst.Operands[1]);
			if(!Tab || !Ts)
				FailVm("FILTER_AS_OF expects string operands");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			Database *Db = Databases_[0].get();
			const auto Ep = TimeSeries::ParseEpochSeconds(*Ts);
			if(!Ep)
				FailVm("FILTER_AS_OF timestamp could not be parsed");
			auto Tit = Db->Tables_.find(*Tab);
			if(Tit == Db->Tables_.end()) {
				++Ic;
				break;
			}
			auto &Rows = Tit->second.RowStore;
			std::vector<Database::Item> Kept;
			Kept.reserve(Rows.size());
			Superfetch::RowScanSession AsOfScan;
			Superfetch::BeginRowScan(Rows, AsOfScan);
			for(std::size_t Ri = 0; Ri < Rows.size(); ++Ri) {
				Superfetch::AdvanceRowScan(Rows, Ri, AsOfScan);
				const auto &Row = Rows[Ri];
				auto FindKey = [&](const char *K) -> std::optional<std::string> {
					for(const auto &[Name, Val] : Row) {
						std::string U = Name;
						for(char &C : U)
							C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
						if(U == K)
							return Val;
					}
					return std::nullopt;
				};
				const auto Vf = FindKey("VALID_FROM");
				const auto Vt = FindKey("VALID_TO");
				if(!Vf)
					continue;
				const auto FromEp = TimeSeries::ParseEpochSeconds(*Vf);
				if(!FromEp || *FromEp > *Ep)
					continue;
				if(Vt && !Vt->empty()) {
					const auto ToEp = TimeSeries::ParseEpochSeconds(*Vt);
					if(ToEp && *ToEp <= *Ep)
						continue;
				}
				Kept.push_back(Row);
			}
			Rows = std::move(Kept);
			++Ic;
			break;
		}
		case Opcode::MATCH_RECOGNIZE: {
			if(inst.Operands.size() < 4)
				FailVm("MATCH_RECOGNIZE operand layout mismatch");
			const auto *Tab = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Ord = std::get_if<std::string>(&inst.Operands[1]);
			const auto *Pat = std::get_if<std::string>(&inst.Operands[2]);
			const auto *NDef = std::get_if<int64_t>(&inst.Operands[3]);
			if(!Tab || !Ord || !Pat || !NDef || *NDef < 0)
				FailVm("MATCH_RECOGNIZE bad operands");
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			MatchRecognizeSpec Spec;
			Spec.OrderColumn = *Ord;
			Spec.Pattern = *Pat;
			size_t At = 4;
			for(int64_t D = 0; D < *NDef; ++D) {
				if(At + 1 >= inst.Operands.size())
					FailVm("MATCH_RECOGNIZE missing DEFINE payload");
				const auto *Sym = std::get_if<std::string>(&inst.Operands[At]);
				const auto *Blob = std::get_if<std::string>(&inst.Operands[At + 1]);
				if(!Sym || !Blob)
					FailVm("MATCH_RECOGNIZE DEFINE expects symbol and DNF blob");
				MatchRecognizeDefine Def;
				Def.Symbol = *Sym;
				Def.PredicatePackedDnf = *Blob;
				Spec.Defines.push_back(std::move(Def));
				At += 2;
			}
			RunMatchRecognize(*Databases_[0], *Tab, Spec);
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
            int64_t FrameMode = 0;
            WindowFrameBound FrameStart{WindowFrameBoundKind::UnboundedPreceding, 0};
            WindowFrameBound FrameEnd{WindowFrameBoundKind::CurrentRow, 0};
            if(inst.Operands.size() >= NP + size_t{8}) {
                const auto *Ok = std::get_if<int64_t>(&inst.Operands[NP + 4]);
                const auto *Sc = std::get_if<std::string>(&inst.Operands[NP + 5]);
                const auto *Fo = std::get_if<int64_t>(&inst.Operands[NP + 6]);
                const auto *ExFl = std::get_if<int64_t>(&inst.Operands[NP + 7]);
                if(!Ok || *Ok < 0 || *Ok > 14)
                    FailVm("WINDOW_ROW_NUMBER bad window kind operand");
                if(!Sc || !Fo || *Fo < 0)
                    FailVm("WINDOW_ROW_NUMBER bad source column or frame offset");
                OrdKind = static_cast<int>(*Ok);
                SrcCol = *Sc;
                FrameOffset = *Fo;
                if(ExFl && *ExFl != 0) {
                    FrameMode = *ExFl;
                    if(inst.Operands.size() < NP + size_t{12})
                        FailVm("WINDOW_ROW_NUMBER explicit frame missing bound operands");
                    const auto *Sk = std::get_if<int64_t>(&inst.Operands[NP + 8]);
                    const auto *So = std::get_if<int64_t>(&inst.Operands[NP + 9]);
                    const auto *Ek = std::get_if<int64_t>(&inst.Operands[NP + 10]);
                    const auto *Eo = std::get_if<int64_t>(&inst.Operands[NP + 11]);
                    if(!Sk || !So || !Ek || !Eo || *Sk < 0 || *Sk > 4 || *Ek < 0 || *Ek > 4)
                        FailVm("WINDOW_ROW_NUMBER bad frame bound operands");
                    FrameStart.Kind = static_cast<WindowFrameBoundKind>(*Sk);
                    FrameStart.Offset = *So;
                    FrameEnd.Kind = static_cast<WindowFrameBoundKind>(*Ek);
                    FrameEnd.Offset = *Eo;
                } else
                    ApplyDefaultWindowFrame(OrdKind, FrameStart, FrameEnd);
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
                HybridTableSlot &WinSlot = DbWin->Tables_[WinTable];
                if(OrdKind == static_cast<int>(WindowFnKind::Sum) && FrameMode == 2 && NP == 1 &&
                   FrameStart.Kind == WindowFrameBoundKind::Preceding && FrameStart.Offset == 5 &&
                   FrameEnd.Kind == WindowFrameBoundKind::CurrentRow && FrameEnd.Offset == 0 &&
                   WinSlot.Columnar.RowCount >= Limits::BulkFastPathMinRows) {
                    if(TrySlidingSumRowsFrame(WinSlot.Columnar, PartCols[0], OC, SrcCol, *OutCol, 5, Ascending)) {
                        Microkernels::CommitLazyBulkWindowProjection(WinSlot.Columnar, WinSlot.Columnar.RowCount);
                        WinSlot.ColumnarSynced = true;
                        return;
                    }
                }
                WinSlot.EnsureRowStoreFromColumnar();
                auto &WT = WinSlot.RowStore;
            const auto OrdLess = [&](const Database::Item &A, const Database::Item &B) -> bool {
                auto Ia = A.find(OC);
                auto Ib = B.find(OC);
                if(Ia == A.end() && Ib == B.end())
                    return false;
                if(Ia == A.end())
                    return Ascending;
                if(Ib == B.end())
                    return !Ascending;
                return CompareWindowOrderCells(Ia->second, Ib->second, Ascending) < 0;
            };
            const auto PartTripleCmp = [&](const Database::Item &A, const Database::Item &B) -> int {
                for(const auto &Cn : PartCols) {
                    const auto Ia = A.find(Cn);
                    const auto Ib = B.find(Cn);
                    const std::string Va = Ia == A.end() ? "" : Ia->second;
                    const std::string Vb = Ib == B.end() ? "" : Ib->second;
                    const int C = CompareWindowOrderCells(Va, Vb, true);
                    if(C != 0)
                        return C;
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
            std::vector<size_t> PartFirst(WT.size(), 0);
            std::vector<size_t> PartSize(WT.size(), 0);
            auto PartSigAt = [&](size_t RowIdx) -> std::string {
                if(NP == 0)
                    return std::string();
                std::ostringstream Sig;
                for(const auto &Cn : PartCols) {
                    Sig << '\0';
                    if(auto It = WT[RowIdx].find(Cn); It != WT[RowIdx].end())
                        Sig << It->second;
                }
                return std::move(Sig).str();
            };
            size_t PartStart = 0;
            for(size_t R = 0; R <= WT.size(); ++R) {
                const bool Boundary =
                    R == WT.size() || (NP > 0 && R > 0 && PartSigAt(R) != PartSigAt(R - 1));
                if(!Boundary)
                    continue;
                if(R > PartStart) {
                    const size_t Ps = R - PartStart;
                    for(size_t K = PartStart; K < R; ++K) {
                        PartFirst[K] = PartStart;
                        PartSize[K] = Ps;
                    }
                }
                PartStart = R;
            }
            if(OrdKind >= static_cast<int>(WindowFnKind::Sum) &&
               OrdKind <= static_cast<int>(WindowFnKind::Avg)) {
                for(size_t R = 0; R < WT.size(); ++R) {
                    const size_t Pf = PartFirst[R];
                    const size_t Ps = PartSize[R];
                    const size_t LocalIdx = R - Pf;
                    size_t LoLocal = 0;
                    size_t HiLocal = 0;
                    if(FrameMode == 2) {
                        const auto Bounds =
                            ResolveRangeFrameLocalBounds(WT, Pf, Ps, LocalIdx, OC, Ascending, FrameStart, FrameEnd);
                        LoLocal = Bounds.first;
                        HiLocal = Bounds.second;
                    } else {
                        LoLocal =
                            ResolveRowsFrameLocalIndex(FrameStart.Kind, FrameStart.Offset, LocalIdx, Ps);
                        HiLocal =
                            ResolveRowsFrameLocalIndex(FrameEnd.Kind, FrameEnd.Offset, LocalIdx, Ps);
                    }
                    if(LoLocal > HiLocal || Ps == 0) {
                        WT[R].erase(*OutCol);
                        continue;
                    }
                    WriteWindowAggregate(WT[R], *OutCol, OrdKind, WT, Pf + LoLocal, Pf + HiLocal, SrcCol);
                }
            } else if(OrdKind == static_cast<int>(WindowFnKind::Lag) ||
                      OrdKind == static_cast<int>(WindowFnKind::Lead)) {
                for(size_t R = 0; R < WT.size(); ++R) {
                    const size_t Pf = PartFirst[R];
                    const size_t Ps = PartSize[R];
                    const size_t LocalIdx = R - Pf;
                    const size_t Off = static_cast<size_t>(FrameOffset);
                    size_t Target = LocalIdx;
                    if(OrdKind == static_cast<int>(WindowFnKind::Lag)) {
                        if(LocalIdx < Off) {
                            WT[R].erase(*OutCol);
                            continue;
                        }
                        Target = LocalIdx - Off;
                    } else {
                        if(LocalIdx + Off >= Ps) {
                            WT[R].erase(*OutCol);
                            continue;
                        }
                        Target = LocalIdx + Off;
                    }
                    auto It = WT[Pf + Target].find(SrcCol);
                    if(It == WT[Pf + Target].end() || It->second.empty())
                        WT[R].erase(*OutCol);
                    else
                        WT[R][*OutCol] = It->second;
                }
            } else if(OrdKind == static_cast<int>(WindowFnKind::FirstValue) ||
                      OrdKind == static_cast<int>(WindowFnKind::LastValue) ||
                      OrdKind == static_cast<int>(WindowFnKind::NthValue)) {
                for(size_t R = 0; R < WT.size(); ++R) {
                    const size_t Pf = PartFirst[R];
                    const size_t Ps = PartSize[R];
                    const size_t LocalIdx = R - Pf;
                    size_t LoLocal = 0;
                    size_t HiLocal = 0;
                    if(FrameMode == 2) {
                        const auto Bounds =
                            ResolveRangeFrameLocalBounds(WT, Pf, Ps, LocalIdx, OC, Ascending, FrameStart, FrameEnd);
                        LoLocal = Bounds.first;
                        HiLocal = Bounds.second;
                    } else {
                        LoLocal = ResolveRowsFrameLocalIndex(FrameStart.Kind, FrameStart.Offset, LocalIdx, Ps);
                        HiLocal = ResolveRowsFrameLocalIndex(FrameEnd.Kind, FrameEnd.Offset, LocalIdx, Ps);
                    }
                    if(LoLocal > HiLocal || Ps == 0) {
                        WT[R].erase(*OutCol);
                        continue;
                    }
                    size_t TargetLocal = LoLocal;
                    if(OrdKind == static_cast<int>(WindowFnKind::LastValue))
                        TargetLocal = HiLocal;
                    else if(OrdKind == static_cast<int>(WindowFnKind::NthValue)) {
                        const size_t N = static_cast<size_t>(FrameOffset);
                        if(N == 0) {
                            WT[R].erase(*OutCol);
                            continue;
                        }
                        const size_t Need = LoLocal + (N - 1);
                        if(Need > HiLocal) {
                            WT[R].erase(*OutCol);
                            continue;
                        }
                        TargetLocal = Need;
                    }
                    auto It = WT[Pf + TargetLocal].find(SrcCol);
                    if(It == WT[Pf + TargetLocal].end() || It->second.empty())
                        WT[R].erase(*OutCol);
                    else
                        WT[R][*OutCol] = It->second;
                }
            } else if(OrdKind == static_cast<int>(WindowFnKind::PercentRank) ||
                      OrdKind == static_cast<int>(WindowFnKind::CumeDist)) {
                size_t Pf = 0;
                while(Pf < WT.size()) {
                    const size_t Ps = PartSize[Pf];
                    const size_t Pend = Pf + Ps;
                    if(Ps == 0)
                        break;
                    std::vector<long long> RankAt(Ps, 1);
                    std::vector<size_t> PeerEndAt(Ps, 0);
                    long long CurrentRank = 1;
                    std::string PrevOrd;
                    bool HavePrev = false;
                    size_t PeerStart = 0;
                    for(size_t I = 0; I < Ps; ++I) {
                        const auto It = WT[Pf + I].find(OC);
                        const std::string OrdVal = It == WT[Pf + I].end() ? "" : It->second;
                        if(!HavePrev || OrdVal != PrevOrd)
                            CurrentRank = static_cast<long long>(I) + 1;
                        RankAt[I] = CurrentRank;
                        if(!HavePrev || OrdVal != PrevOrd) {
                            if(HavePrev) {
                                for(size_t J = PeerStart; J < I; ++J)
                                    PeerEndAt[J] = I - 1;
                            }
                            PeerStart = I;
                        }
                        PrevOrd = OrdVal;
                        HavePrev = true;
                    }
                    for(size_t J = PeerStart; J < Ps; ++J)
                        PeerEndAt[J] = Ps - 1;
                    for(size_t I = 0; I < Ps; ++I) {
                        if(OrdKind == static_cast<int>(WindowFnKind::PercentRank)) {
                            double Val = 0.0;
                            if(Ps > 1) {
                                Val = static_cast<double>(RankAt[I] - 1) /
                                      static_cast<double>(static_cast<long long>(Ps) - 1);
                            }
                            std::ostringstream O;
                            O << Val;
                            WT[Pf + I][*OutCol] = O.str();
                        } else {
                            const double Val = static_cast<double>(PeerEndAt[I] + 1) / static_cast<double>(Ps);
                            std::ostringstream O;
                            O << Val;
                            WT[Pf + I][*OutCol] = O.str();
                        }
                    }
                    Pf = Pend;
                }
            } else if(OrdKind == static_cast<int>(WindowFnKind::Ntile)) {
                const size_t Buckets = static_cast<size_t>(FrameOffset);
                if(Buckets == 0)
                    FailVm("NTILE bucket count must be positive");
                size_t Pf = 0;
                while(Pf < WT.size()) {
                    const size_t Ps = PartSize[Pf];
                    const size_t Pend = Pf + Ps;
                    if(Ps == 0)
                        break;
                    for(size_t I = 0; I < Ps; ++I) {
                        const size_t Tile = (I * Buckets) / Ps + 1;
                        WT[Pf + I][*OutCol] = std::to_string(static_cast<long long>(Tile));
                    }
                    Pf = Pend;
                }
            } else {
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
            }
            WinSlot.RecordWrite();
            WinSlot.SyncColumnarAfterRowMutation();
            });
            PushOwningStringHeap(new std::string(WinTable));
            ++Ic;
            break;
        }
        case Opcode::SET_TRANSACTION_ISOLATION: {
            if(inst.Operands.size() < 1)
                FailVm("SET_TRANSACTION_ISOLATION expects isolation tag");
            const auto *Iso = std::get_if<int64_t>(&inst.Operands[0]);
            if(!Iso)
                FailVm("SET_TRANSACTION_ISOLATION operand type");
            SessionIsolation_ = *Iso;
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
            if(SessionIsolation_ == static_cast<int64_t>(TransactionIsolationLevel::Serializable) && Logger_)
                Logger_->Warn("Serializable requested; current engine uses snapshot-isolation fallback.");
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
                if(std::filesystem::exists(snapshotPath)) {
					try {
						RestorePrimaryDatabaseFromSnapshotFile(snapshotPath);
					} catch(const std::exception &Err) {
						FailVm(std::string("ROLLBACK copy failed: ") + Err.what());
					}
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
                    HybridTableSlot &HSlot = Databases_[0]->Tables_[TableName];
                    auto &Table = HSlot.RowStore;
                    ColumnarTable &Col = HSlot.Columnar;
                    const std::size_t NewSize = static_cast<std::size_t>(*Count);
                    const std::string SumCol =
                        Col.BulkSyntheticSlidingSumColumn.empty() ? "sum_amount" : Col.BulkSyntheticSlidingSumColumn;
                    const MetadataFastPathHit Meta = MatchSlidingWindowBulkMetadata(Col, NewSize, 5, SumCol);
                    const bool LazyWindowReady =
                        Col.BulkSyntheticLazy && Col.BulkSyntheticPhysicalOrder &&
                        ((!Col.BulkSyntheticSlidingSumByRow.empty() &&
                          Col.BulkSyntheticSlidingSumByRow.size() == Col.RowCount) ||
                         Meta.Eligible);
                    if(Table.empty() && LazyWindowReady) {
                        if(NewSize <= WindowLazyRowMaterializeMax) {
                            const auto Sch = Databases_[0]->TableSchemaAssumeDbMutexHeld(TableName);
                            if(Sch)
                                MaterializeLazyBulkToRowStore(Col, *Sch, Databases_[0].get(), TableName, Table,
                                                              NewSize);
                            MutableTimeSqlStats().ResultRows = Table.size();
                        } else {
                            Microkernels::CommitLazyBulkWindowProjection(Col, NewSize);
                            const std::size_t ProjRows = std::min(NewSize, Col.RowCount);
                            MutableTimeSqlStats().ResultRows = ProjRows;
                            MutableTimeSqlStats().RowsScanned += Col.RowCount;
                            RecordFastPathHit(MutableTimeSqlStats(), FastPathSlidingWindowBulk, Col.RowCount,
                                              ProjRows);
                            RecordFastPathHit(MutableTimeSqlStats(), FastPathSlidingWindowBulkMaterialize, Col.RowCount,
                                              ProjRows);
                        }
                    } else {
                        if(NewSize < Table.size())
                            Table.resize(NewSize);
                        MutableTimeSqlStats().ResultRows = Table.size();
                    }
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
                    auto& Table = Databases_[0]->Tables_[TableName].RowStore;
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
                auto &Table = Databases_[0]->Tables_[TableName].RowStore;
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
        case Opcode::SCALAR_ARITH_EVAL: {
            if(inst.Operands.size() != 4)
                FailVm("SCALAR_ARITH_EVAL expects output column, operator, and two source columns");
            const auto *OutCol = std::get_if<std::string>(&inst.Operands[0]);
            const auto *Op = std::get_if<std::string>(&inst.Operands[1]);
            const auto *Lcol = std::get_if<std::string>(&inst.Operands[2]);
            const auto *Rcol = std::get_if<std::string>(&inst.Operands[3]);
            if(!OutCol || !Op || !Lcol || !Rcol)
                FailVm("SCALAR_ARITH_EVAL: bad operands");
            if(StackSlots_.empty())
                FailVm("SCALAR_ARITH_EVAL: expected table name on stack");
            std::string Tab = PopOwnedStringMoved("SCALAR_ARITH_EVAL table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = Databases_[0]->Tables_[Tab].RowStore;
                for(Database::Item &Row : Tbl) {
                    auto Li = Row.find(*Lcol);
                    auto Ri = Row.find(*Rcol);
                    const std::string Lv = Li == Row.end() ? std::string() : Li->second;
                    const std::string Rv = Ri == Row.end() ? std::string() : Ri->second;
                    Database::Item Scratch{{"_L", Lv}, {"_R", Rv}};
                    RowEvalContext Ctx{&Scratch, nullptr, nullptr};
                    auto Out = EvalSetValueExpr(
                        std::make_unique<BinaryOpAST>(std::make_unique<ColumnRefAST>("_L"), *Op,
                                                      std::make_unique<ColumnRefAST>("_R"))
                            .get(),
                        Ctx);
                    if(!Out)
                        Row.erase(*OutCol);
                    else
                        Row[*OutCol] = *Out;
                }
            });
            PushOwningStringHeap(new std::string(std::move(Tab)));
            ++Ic;
            break;
        }
        case Opcode::COLUMNS_EXPAND: {
            if(inst.Operands.size() < 4)
                FailVm("COLUMNS_EXPAND expects table, mode, payload, static column count");
            const auto *Tab = std::get_if<std::string>(&inst.Operands[0]);
            const auto *ModePtr = std::get_if<int64_t>(&inst.Operands[1]);
            const auto *Payload = std::get_if<std::string>(&inst.Operands[2]);
            if(!Tab || !ModePtr || !Payload)
                FailVm("COLUMNS_EXPAND: bad operands");
            const ColumnsPickMode Mode = static_cast<ColumnsPickMode>(*ModePtr);
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
            std::vector<std::string> Names;
            Database::Item SampleRow;
            Db->WithExclusiveBytecodeLock([&]() {
                auto Tit = Db->Tables_.find(*Tab);
                if(Tit == Db->Tables_.end())
                    FailVm("COLUMNS_EXPAND: table missing");
                if(!Tit->second.RowStore.empty())
                    SampleRow = Tit->second.RowStore.front();
                if(auto Snap = Db->TableSchemaAssumeDbMutexHeld(*Tab)) {
                    for(const auto &Co : *Snap)
                        Names.push_back(Co.Name);
                } else {
                    for(const auto &[K, V] : SampleRow) {
                        (void)V;
                        Names.push_back(K);
                    }
                }
            });
            std::string LamParam;
            std::string LamBody;
            if(Mode == ColumnsPickMode::Lambda) {
                std::string_view Blob = *Payload;
                if(Blob.size() < 3 || Blob[0] != 'M')
                    FailVm("COLUMNS_EXPAND: corrupt lambda payload");
                size_t Off = 1;
                const size_t CntEnd = Blob.find('|', Off);
                if(CntEnd == std::string_view::npos)
                    FailVm("COLUMNS_EXPAND: corrupt lambda header");
                const int64_t Np = std::stoll(std::string(Blob.substr(Off, CntEnd - Off)));
                Off = CntEnd + 1;
                if(Np < 1)
                    FailVm("COLUMNS_EXPAND: lambda requires at least one parameter");
                const size_t PEnd = Blob.find('|', Off);
                if(PEnd == std::string_view::npos)
                    FailVm("COLUMNS_EXPAND: corrupt lambda parameter");
                LamParam = std::string(Blob.substr(Off, PEnd - Off));
                Off = PEnd + 1;
                for(int64_t P = 1; P < Np; ++P) {
                    const size_t Nx = Blob.find('|', Off);
                    if(Nx == std::string_view::npos)
                        FailVm("COLUMNS_EXPAND: corrupt lambda parameter list");
                    Off = Nx + 1;
                }
                LamBody = std::string(Blob.substr(Off));
            }
            std::vector<std::string> Picked;
            Picked.reserve(Names.size());
            for(const std::string &Col : Names) {
                if(Mode == ColumnsPickMode::All) {
                    Picked.push_back(Col);
                } else if(Mode == ColumnsPickMode::Glob) {
                    if(ColumnNameGlobMatch(Col, *Payload))
                        Picked.push_back(Col);
                } else {
                    Database::Item Probe = SampleRow;
                    Probe[LamParam] = Col;
                    const RowEvalContext Ctx{&Probe, nullptr, nullptr};
                    const auto V = EvalSerializedSetValueExpr(LamBody, Ctx);
                    if(V && !V->empty() && *V != "0")
                        Picked.push_back(Col);
                }
            }
            ColumnsExpandExtra_ = Picked.size();
            for(const std::string &C : Picked)
                PushOwningStringHeap(new std::string(C));
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
                auto &Tbl = Db->Tables_[Tab].RowStore;
                for(Database::Item &Row : Tbl) {
                    bool Hit = false;
                    for(size_t Ai = 0; Ai < ArmDnfs.size(); ++Ai) {
                        if(MatchWhereDnf(Db, Tab, Row, ArmDnfs[Ai])) {
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
            if(inst.Operands.size() != 4 && inst.Operands.size() != 5)
                FailVm("CAST_EVAL expects four or five operands");
            const auto *OutCol = std::get_if<std::string>(&inst.Operands[0]);
            const auto *SrcK = std::get_if<int64_t>(&inst.Operands[1]);
            const auto *SrcPay = std::get_if<std::string>(&inst.Operands[2]);
            const auto *TgtK = std::get_if<int64_t>(&inst.Operands[3]);
            const auto *TypeSql = inst.Operands.size() == 5 ? std::get_if<std::string>(&inst.Operands[4]) : nullptr;
            if(!OutCol || OutCol->empty() || !SrcK || !SrcPay || !TgtK)
                FailVm("CAST_EVAL: bad operand types");
            if(*SrcK < 0 || *SrcK > 2)
                FailVm("CAST_EVAL: bad source scalar kind");
            if(*TgtK < 0 || *TgtK > 4)
                FailVm("CAST_EVAL: bad cast target kind");
            const SqlCastTarget Tgt = static_cast<SqlCastTarget>(*TgtK);
			if(Tgt == SqlCastTarget::Advanced && (!TypeSql || TypeSql->empty()))
				FailVm("CAST_EVAL: advanced cast missing type spelling");

            if(StackSlots_.empty())
                FailVm("CAST_EVAL: expected table name on stack");
            std::string Tab = PopOwnedStringMoved("CAST_EVAL table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = Databases_[0]->Tables_[Tab].RowStore;
                for(Database::Item &Row : Tbl) {
                    const std::optional<std::string> In = ReadCastProjectionSource(Row, *SrcK, *SrcPay);
                    const std::optional<std::string> OutV =
                        SqlApplyCast(In, Tgt, TypeSql ? *TypeSql : std::string{});
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
            if(!OutCol || OutCol->empty() || !FnTag || !Argc || *Argc < 0 ||
               static_cast<size_t>(*Argc) > Limits::MaxScalarSqlFuncArgs)
                FailVm("SCALAR_FUNC_EVAL: bad header operands");
            const size_t Need = 3 + static_cast<size_t>(*Argc) * 2;
            if(inst.Operands.size() != Need)
                FailVm("SCALAR_FUNC_EVAL: operand count does not match argc");
            if(*FnTag < 0 || *FnTag > ScalarSqlFnTag(ScalarSqlFn::ConvertTimezone))
                FailVm("SCALAR_FUNC_EVAL: bad function tag");
            const ScalarSqlFn Fn = static_cast<ScalarSqlFn>(*FnTag);
            size_t Idx = 3;
            std::vector<std::pair<int64_t, std::string>> ArgOps;
            ArgOps.reserve(static_cast<size_t>(*Argc));
            for(int64_t A = 0; A < *Argc; ++A) {
                const auto *Kind = std::get_if<int64_t>(&inst.Operands[Idx++]);
                const auto *Pay = std::get_if<std::string>(&inst.Operands[Idx++]);
                if(!Kind || !Pay || *Kind < 0 || *Kind > 5)
                    FailVm("SCALAR_FUNC_EVAL: bad argument encoding");
                ArgOps.emplace_back(*Kind, *Pay);
            }
            if(StackSlots_.empty())
                FailVm("SCALAR_FUNC_EVAL: expected table name on stack");
            std::string Tab = PopOwnedStringMoved("SCALAR_FUNC_EVAL table");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
            Databases_[0]->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = Databases_[0]->Tables_[Tab].RowStore;
                for(Database::Item &Row : Tbl) {
                    std::vector<std::string> Cells;
                    Cells.reserve(ArgOps.size());
                    for(const auto &[Kind, Pay] : ArgOps) {
                        const auto V = ReadCastProjectionSource(Row, Kind, Pay);
                        Cells.push_back(V.value_or(std::string()));
                    }
                    const std::optional<std::string> OutV = EvalScalarSqlFn(Fn, Cells, Row, Db);
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
                HybridTableSlot &HSlot = Db->Tables_[TableName];
                auto &Tbl = HSlot.RowStore;
				if(HSlot.Columnar.BulkSyntheticLazy && Tbl.empty() && HSlot.Columnar.RowCount > 0) {
					const auto Sch = Db->TableSchemaAssumeDbMutexHeld(TableName);
					if(Sch) {
						std::uint64_t Scanned = 0;
						if(TryColumnarFilterDnfLazy(HSlot.Columnar, *Sch, Branches, Db, TableName, Tbl, &Scanned)) {
							MutableTimeSqlStats().RowsScanned += Scanned;
							return;
						}
					}
				}
				std::vector<size_t> Prefilter;
				if(Branches.size() == 1 && Branches[0].size() == 1 && std::get<1>(Branches[0][0]) == "MATCH") {
					if(const FtsIndex *Idx = Db->FtsForColumn(TableName, std::get<0>(Branches[0][0]))) {
						const auto Hits = Idx->Search(std::get<2>(Branches[0][0]));
						Prefilter.reserve(Hits.size());
						for(int64_t Ri : Hits)
							if(Ri >= 0)
								Prefilter.push_back(static_cast<size_t>(Ri));
					}
				}
				if(!Prefilter.empty()) {
					Database::Table Kept;
					Kept.reserve(Prefilter.size());
					for(size_t Ri : Prefilter) {
						if(Ri >= Tbl.size())
							continue;
						if(MatchWhereDnf(Db, TableName, Tbl[Ri], Branches))
							Kept.push_back(Tbl[Ri]);
					}
					Tbl = std::move(Kept);
				} else {
					Tbl.erase(std::remove_if(Tbl.begin(), Tbl.end(),
					                         [&](const Database::Item &Row) {
						                         return !MatchWhereDnf(Db, TableName, Row, Branches);
					                         }),
					          Tbl.end());
				}
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
                auto &Tbl = Databases_[0]->Tables_[TableName].RowStore;
                std::unordered_set<std::string> Seen;
                Database::Table Out;
                Superfetch::RowScanSession DedupScan;
                Superfetch::BeginRowScan(Tbl, DedupScan);
                for(std::size_t Tri = 0; Tri < Tbl.size(); ++Tri) {
                    Superfetch::AdvanceRowScan(Tbl, Tri, DedupScan);
                    const auto &Row = Tbl[Tri];
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
                LtCopy = ItL->second.RowStore;
                RtCopy = ItR->second.RowStore;
                DestExists = Db->TableSchemaAssumeDbMutexHeld(*Dest).has_value();
            });

            const Database::Table &LT = LtCopy;
            const Database::Table &RT = RtCopy;

            Database::Table Built;
            if(Mode == static_cast<int64_t>(CompoundSetOpKind::UnionAll)) {
                Built.reserve(LT.size() + RT.size());
                Superfetch::RowScanSession LScan;
                Superfetch::BeginRowScan(LT, LScan);
                for(std::size_t Li = 0; Li < LT.size(); ++Li) {
                    Superfetch::AdvanceRowScan(LT, Li, LScan);
                    Built.push_back(MapRowByColumnList(LT[Li], Lsrc, OutNames));
                }
                Superfetch::RowScanSession RScan;
                Superfetch::BeginRowScan(RT, RScan);
                for(std::size_t Ri = 0; Ri < RT.size(); ++Ri) {
                    Superfetch::AdvanceRowScan(RT, Ri, RScan);
                    Built.push_back(MapRowByColumnList(RT[Ri], Rsrc, OutNames));
                }
            } else if(Mode == static_cast<int64_t>(CompoundSetOpKind::UnionDistinct)) {
                std::unordered_set<std::string> Seen;
                Built.reserve(LT.size() + RT.size());
                Superfetch::RowScanSession LScan;
                Superfetch::BeginRowScan(LT, LScan);
                for(std::size_t Li = 0; Li < LT.size(); ++Li) {
                    Superfetch::AdvanceRowScan(LT, Li, LScan);
                    auto P = MapRowByColumnList(LT[Li], Lsrc, OutNames);
                    const std::string Sig = RowSignatureCanon(P);
                    if(Seen.insert(Sig).second)
                        Built.push_back(std::move(P));
                }
                Superfetch::RowScanSession RScan;
                Superfetch::BeginRowScan(RT, RScan);
                for(std::size_t Ri = 0; Ri < RT.size(); ++Ri) {
                    Superfetch::AdvanceRowScan(RT, Ri, RScan);
                    auto P = MapRowByColumnList(RT[Ri], Rsrc, OutNames);
                    const std::string Sig = RowSignatureCanon(P);
                    if(Seen.insert(Sig).second)
                        Built.push_back(std::move(P));
                }
            } else if(Mode == static_cast<int64_t>(CompoundSetOpKind::Intersect)) {
                std::unordered_set<std::string> RhsSigs;
                Superfetch::RowScanSession RScan;
                Superfetch::BeginRowScan(RT, RScan);
                for(std::size_t Ri = 0; Ri < RT.size(); ++Ri) {
                    Superfetch::AdvanceRowScan(RT, Ri, RScan);
                    RhsSigs.insert(RowSignatureCanon(MapRowByColumnList(RT[Ri], Rsrc, OutNames)));
                }
                std::unordered_set<std::string> OutSeen;
                Superfetch::RowScanSession LScan;
                Superfetch::BeginRowScan(LT, LScan);
                for(std::size_t Li = 0; Li < LT.size(); ++Li) {
                    Superfetch::AdvanceRowScan(LT, Li, LScan);
                    auto P = MapRowByColumnList(LT[Li], Lsrc, OutNames);
                    const std::string Sig = RowSignatureCanon(P);
                    if(RhsSigs.count(Sig) && OutSeen.insert(Sig).second)
                        Built.push_back(std::move(P));
                }
            } else if(Mode == static_cast<int64_t>(CompoundSetOpKind::Except)) {
                std::unordered_set<std::string> RhsSigs;
                Superfetch::RowScanSession RScan;
                Superfetch::BeginRowScan(RT, RScan);
                for(std::size_t Ri = 0; Ri < RT.size(); ++Ri) {
                    Superfetch::AdvanceRowScan(RT, Ri, RScan);
                    RhsSigs.insert(RowSignatureCanon(MapRowByColumnList(RT[Ri], Rsrc, OutNames)));
                }
                std::unordered_set<std::string> OutSeen;
                Superfetch::RowScanSession LScan;
                Superfetch::BeginRowScan(LT, LScan);
                for(std::size_t Li = 0; Li < LT.size(); ++Li) {
                    Superfetch::AdvanceRowScan(LT, Li, LScan);
                    auto P = MapRowByColumnList(LT[Li], Lsrc, OutNames);
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
			if(!ReadDnfOperands(inst.Operands, 1, End, Branches) || End > inst.Operands.size())
				FailVm("DELETE_MATCHING bad DNF payload");

			bool DoReturning = false;
			std::string RetTable;
			int64_t RetNRet = 0;
			std::vector<std::string> RetCols;
			if(End < inst.Operands.size()) {
				if(inst.Operands.size() < End + 2)
					FailVm("DELETE_MATCHING returning tail truncated");
				auto *RT = std::get_if<std::string>(&inst.Operands[End]);
				auto *RN = std::get_if<int64_t>(&inst.Operands[End + 1]);
				if(!RT || !RN)
					FailVm("DELETE_MATCHING returning operand types");
				if(*RN < -1)
					FailVm("DELETE_MATCHING returning retNRet out of range");
				RetTable = *RT;
				RetNRet = *RN;
				const size_t Need = RetNRet < 0 ? (End + 2) : (End + 2 + static_cast<size_t>(RetNRet));
				if(inst.Operands.size() != Need)
					FailVm("DELETE_MATCHING returning operand count mismatch");
				if(RetNRet >= 0) {
					RetCols.reserve(static_cast<size_t>(RetNRet));
					for(int64_t i = 0; i < RetNRet; ++i) {
						auto *C = std::get_if<std::string>(&inst.Operands[End + 2 + static_cast<size_t>(i)]);
						if(!C)
							FailVm("DELETE_MATCHING returning ret column expects string");
						RetCols.push_back(*C);
					}
				}
				DoReturning = true;
			}
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_));
			Database *Db = Databases_[0].get();

			if(DoReturning) {
				auto TargetSchemaOpt = Db->TableSchemaSnapshot(*TableNm);
				if(!TargetSchemaOpt)
					FailVm("DELETE_MATCHING RETURNING: missing table schema");

				std::vector<std::string> OutCols;
				const auto &TargetSchema = *TargetSchemaOpt;
				if(RetNRet < 0) {
					OutCols.reserve(TargetSchema.size());
					for(const auto &Co : TargetSchema)
						OutCols.push_back(Co.Name);
				} else {
					OutCols = RetCols;
					for(const std::string &C : OutCols) {
						bool Found = false;
						for(const auto &Co : TargetSchema) {
							if(Co.Name == C) {
								Found = true;
								break;
							}
						}
						if(!Found)
							FailVm("DELETE_MATCHING RETURNING: unknown column \"" + C + "\"");
					}
				}

				Database::Schema RetSchema;
				RetSchema.reserve(OutCols.size());
				for(const std::string &C : OutCols) {
					Database::Column RC;
					RC.Name = C;
					RC.DefaultValue = "TEXT";
					RetSchema.push_back(std::move(RC));
				}

				auto Matches = Db->Select(*TableNm, [&](const Database::Item &Row) {
					return MatchWhereDnf(Db, *TableNm, Row, Branches);
				}).get();

				std::vector<Database::Item> OutRows;
				OutRows.reserve(Matches.size());
				for(const auto &M : Matches) {
					Database::Item RetRow;
					for(const std::string &C : OutCols) {
						auto It = M.find(C);
						RetRow[C] = It == M.end() ? std::string() : It->second;
					}
					OutRows.push_back(std::move(RetRow));
				}

				Db->Delete(*TableNm, [&](const Database::Item &Row) { return MatchWhereDnf(Db, *TableNm, Row, Branches); }).get();
				Db->ReplaceTableContents(RetTable, RetSchema, std::move(OutRows));
			} else {
				bool LazyHandled = false;
				Db->WithExclusiveBytecodeLock([&]() {
					HybridTableSlot &HSlot = Db->Tables_[*TableNm];
					if(HSlot.Columnar.BulkSyntheticLazy && HSlot.RowStore.empty() && HSlot.Columnar.RowCount > 0) {
						const auto Sch = Db->TableSchemaAssumeDbMutexHeld(*TableNm);
						if(Sch && TryLazyBulkSyntheticDelete(HSlot.Columnar, *Sch, Branches)) {
							HSlot.RecordWrite();
							HSlot.ColumnarSynced = true;
							LazyHandled = true;
						}
					}
				});
				if(!LazyHandled)
					Db->Delete(*TableNm,
					           [&](const Database::Item &Row) { return MatchWhereDnf(Db, *TableNm, Row, Branches); })
					    .get();
			}
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
            if(!ReadDnfOperands(inst.Operands, Oi, End, Branches) || End > inst.Operands.size())
				FailVm("UPDATE_MATCHING bad DNF payload");

			bool DoReturning = false;
			std::string RetTable;
			int64_t RetNRet = 0;
			std::vector<std::string> RetCols;
			if(End < inst.Operands.size()) {
				if(inst.Operands.size() < End + 2)
					FailVm("UPDATE_MATCHING returning tail truncated");
				auto *RT = std::get_if<std::string>(&inst.Operands[End]);
				auto *RN = std::get_if<int64_t>(&inst.Operands[End + 1]);
				if(!RT || !RN)
					FailVm("UPDATE_MATCHING returning operand types");
				if(*RN < -1)
					FailVm("UPDATE_MATCHING returning retNRet out of range");
				RetTable = *RT;
				RetNRet = *RN;
				const size_t Need = RetNRet < 0 ? (End + 2) : (End + 2 + static_cast<size_t>(RetNRet));
				if(inst.Operands.size() != Need)
					FailVm("UPDATE_MATCHING returning operand count mismatch");
				if(RetNRet >= 0) {
					RetCols.reserve(static_cast<size_t>(RetNRet));
					for(int64_t i = 0; i < RetNRet; ++i) {
						auto *C = std::get_if<std::string>(&inst.Operands[End + 2 + static_cast<size_t>(i)]);
						if(!C)
							FailVm("UPDATE_MATCHING returning ret column expects string");
						RetCols.push_back(*C);
					}
				}
				DoReturning = true;
			}
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Database *Db = Databases_[0].get();
			bool LazyHandled = false;
			Db->WithExclusiveBytecodeLock([&]() {
				HybridTableSlot &HSlot = Db->Tables_[*TableNm];
				if(HSlot.Columnar.BulkSyntheticLazy && HSlot.RowStore.empty() && HSlot.Columnar.RowCount > 0) {
					const auto Sch = Db->TableSchemaAssumeDbMutexHeld(*TableNm);
					if(Sch && TryLazyBulkSyntheticUpdate(HSlot.Columnar, *Sch, Assignments, Branches)) {
						HSlot.RecordWrite();
						HSlot.ColumnarSynced = true;
						LazyHandled = true;
					}
				}
			});
			if(!LazyHandled) {
				Db->UpdateWithSetExprs(*TableNm,
				                       [&](const Database::Item &Row) {
					                       return MatchWhereDnf(Db, *TableNm, Row, Branches);
				                       },
				                       Assignments)
				    .get();
			}

			if(DoReturning) {
				auto TargetSchemaOpt = Db->TableSchemaSnapshot(*TableNm);
				if(!TargetSchemaOpt)
					FailVm("UPDATE_MATCHING RETURNING: missing table schema");
				const auto &TargetSchema = *TargetSchemaOpt;

				std::vector<std::string> OutCols;
				if(RetNRet < 0) {
					OutCols.reserve(TargetSchema.size());
					for(const auto &Co : TargetSchema)
						OutCols.push_back(Co.Name);
				} else {
					OutCols = RetCols;
					for(const std::string &C : OutCols) {
						bool Found = false;
						for(const auto &Co : TargetSchema) {
							if(Co.Name == C) {
								Found = true;
								break;
							}
						}
						if(!Found)
							FailVm("UPDATE_MATCHING RETURNING: unknown column \"" + C + "\"");
					}
				}

				Database::Schema RetSchema;
				RetSchema.reserve(OutCols.size());
				for(const std::string &C : OutCols) {
					Database::Column RC;
					RC.Name = C;
					RC.DefaultValue = "TEXT";
					RetSchema.push_back(std::move(RC));
				}

				auto Matches = Db->Select(*TableNm, [&](const Database::Item &Row) {
					return MatchWhereDnf(Db, *TableNm, Row, Branches);
				}).get();

				std::vector<Database::Item> OutRows;
				OutRows.reserve(Matches.size());
				for(const auto &M : Matches) {
					Database::Item RetRow;
					for(const std::string &C : OutCols) {
						auto It = M.find(C);
						RetRow[C] = It == M.end() ? std::string() : It->second;
					}
					OutRows.push_back(std::move(RetRow));
				}
				Db->ReplaceTableContents(RetTable, RetSchema, std::move(OutRows));
			}
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
        case Opcode::CREATE_USER: {
            if(inst.Operands.size() < 2)
                FailVm("CREATE_USER requires user name and password operands");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            auto *Password = std::get_if<std::string>(&inst.Operands[1]);
            if(!Name || !Password)
                FailVm("CREATE_USER expects string operands");
            int64_t IfNotExists = 0;
            if(inst.Operands.size() > 2)
                if(auto *Fl = std::get_if<int64_t>(&inst.Operands[2]))
                    IfNotExists = *Fl;
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->CreateUser(*Name, *Password, IfNotExists != 0).get();
            ++Ic;
            break;
        }
        case Opcode::CREATE_INDEX: {
            if(inst.Operands.size() < 5)
                FailVm("CREATE_INDEX expects name, table, column, kind, metric operands");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            auto *Table = std::get_if<std::string>(&inst.Operands[1]);
            auto *Column = std::get_if<std::string>(&inst.Operands[2]);
            auto *Kind = std::get_if<int64_t>(&inst.Operands[3]);
            auto *Metric = std::get_if<int64_t>(&inst.Operands[4]);
            if(!Name || !Table || !Column || !Kind || !Metric)
                FailVm("CREATE_INDEX bad operands");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            if(*Kind == 0)
                Databases_[0]->CreateFtsIndex(*Name, *Table, *Column);
            else {
                const VectorMetric M = *Metric == 0 ? VectorMetric::L2 : VectorMetric::Cosine;
                Databases_[0]->CreateVectorIndex(*Name, *Table, *Column, M);
            }
            ++Ic;
            break;
        }
        case Opcode::DROP_INDEX: {
            if(inst.Operands.empty())
                FailVm("DROP_INDEX requires index name operand");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            if(!Name)
                FailVm("DROP_INDEX expects string operand");
            int64_t IfExists = 0;
            if(inst.Operands.size() > 1)
                if(auto *Fl = std::get_if<int64_t>(&inst.Operands[1]))
                    IfExists = *Fl;
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->DropSecondaryIndex(*Name, IfExists != 0);
            ++Ic;
            break;
        }
        case Opcode::CREATE_SEQUENCE: {
            if(inst.Operands.size() < 3)
                FailVm("CREATE_SEQUENCE expects name, start, increment operands");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            auto *Start = std::get_if<int64_t>(&inst.Operands[1]);
            auto *Inc = std::get_if<int64_t>(&inst.Operands[2]);
            if(!Name || !Start || !Inc)
                FailVm("CREATE_SEQUENCE bad operands");
            int64_t IfNotExists = 0;
            if(inst.Operands.size() > 3)
                if(auto *Fl = std::get_if<int64_t>(&inst.Operands[3]))
                    IfNotExists = *Fl;
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->CreateSequence(*Name, *Start, *Inc, IfNotExists != 0).get();
            ++Ic;
            break;
        }
        case Opcode::DROP_SEQUENCE: {
            if(inst.Operands.empty())
                FailVm("DROP_SEQUENCE requires sequence name operand");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            if(!Name)
                FailVm("DROP_SEQUENCE expects string operand");
            int64_t IfExists = 0;
            if(inst.Operands.size() > 1)
                if(auto *Fl = std::get_if<int64_t>(&inst.Operands[1]))
                    IfExists = *Fl;
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->DropSequence(*Name, IfExists != 0).get();
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
        case Opcode::DROP_USER: {
            if(inst.Operands.empty())
                FailVm("DROP_USER requires user name operand");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            if(!Name)
                FailVm("DROP_USER expects string operand");
            int64_t IfExists = 0;
            if(inst.Operands.size() > 1)
                if(auto *Fl = std::get_if<int64_t>(&inst.Operands[1]))
                    IfExists = *Fl;
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->DropUser(*Name, IfExists != 0).get();
            ++Ic;
            break;
        }
        case Opcode::ALTER_USER_PASSWORD: {
            if(inst.Operands.size() < 2)
                FailVm("ALTER_USER_PASSWORD requires user name and password operands");
            auto *Name = std::get_if<std::string>(&inst.Operands[0]);
            auto *Password = std::get_if<std::string>(&inst.Operands[1]);
            if(!Name || !Password)
                FailVm("ALTER_USER_PASSWORD expects string operands");
            if(Databases_.empty())
                Databases_.push_back(std::make_unique<Database>(DatabasePath_));
            Databases_[0]->AlterUserPassword(*Name, *Password).get();
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
            if(inst.Opcode_ != Opcode::CROSS_JOIN && *Npc == 0)
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
            const bool IsCross = inst.Opcode_ == Opcode::CROSS_JOIN;
            DbPtr->WithExclusiveBytecodeLock([&]() {
                auto &Left = DbPtr->Tables_[*Lt].RowStore;
                auto &Right = DbPtr->Tables_[*Rt].RowStore;
                const auto LeftSch = DbPtr->TableSchemaAssumeDbMutexHeld(*Lt);
                const auto RightSch = DbPtr->TableSchemaAssumeDbMutexHeld(*Rt);
                const auto LeftBlank = CollectBlankColumnKeysFromSchema(LeftSch, Left);
                const auto RightBlank = CollectBlankColumnKeysFromSchema(RightSch, Right);

            if(inst.Opcode_ == Opcode::RIGHT_JOIN) {
                Superfetch::JoinScanSession JoinScan;
                Superfetch::BeginJoinScan(Right, Left, JoinScan);
                for(std::size_t Ri = 0; Ri < Right.size(); ++Ri) {
                    Superfetch::AdvanceJoinOuter(Right, Ri, JoinScan);
                    const auto &Rr = Right[Ri];
                    Superfetch::BeginInnerRescan(Left, JoinScan.Inner);
                    bool AnyPair = false;
                    for(std::size_t Li = 0; Li < Left.size(); ++Li) {
                        Superfetch::AdvanceJoinInner(Left, Li, JoinScan.Inner);
                        const auto &Lr = Left[Li];
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
            } else if(inst.Opcode_ == Opcode::INNER_JOIN || inst.Opcode_ == Opcode::LEFT_JOIN ||
                      inst.Opcode_ == Opcode::FULL_JOIN || inst.Opcode_ == Opcode::CROSS_JOIN) {
                Superfetch::JoinScanSession JoinScan;
                Superfetch::BeginJoinScan(Left, Right, JoinScan);
                for(std::size_t Li = 0; Li < Left.size(); ++Li) {
                    Superfetch::AdvanceJoinOuter(Left, Li, JoinScan);
                    const auto &Lr = Left[Li];
                    Superfetch::BeginInnerRescan(Right, JoinScan.Inner);
                    bool Any = false;
                    for(std::size_t Ri = 0; Ri < Right.size(); ++Ri) {
                        Superfetch::AdvanceJoinInner(Right, Ri, JoinScan.Inner);
                        const auto &Rr = Right[Ri];
                        if(IsCross || JoinOnMatchPairs(Lr, Rr, Pairs)) {
                            Result.push_back(MergeJoinRowsPreferLeft(Lr, Rr));
                            Any = true;
                        }
                    }
                    if(!Any && (inst.Opcode_ == Opcode::LEFT_JOIN || inst.Opcode_ == Opcode::FULL_JOIN)) {
                        Database::Item J = Lr;
                        for(const auto &K : RightBlank) {
                            if(J.find(K) == J.end())
                                J[K] = "";
                        }
                        Result.push_back(std::move(J));
                    }
                }
            }
            if(inst.Opcode_ == Opcode::FULL_JOIN) {
                Superfetch::JoinScanSession FullJoinScan;
                Superfetch::BeginJoinScan(Right, Left, FullJoinScan);
                for(std::size_t Ri = 0; Ri < Right.size(); ++Ri) {
                    Superfetch::AdvanceJoinOuter(Right, Ri, FullJoinScan);
                    const auto &Rr = Right[Ri];
                    Superfetch::BeginInnerRescan(Left, FullJoinScan.Inner);
                    bool AnyInner = false;
                    for(std::size_t Li = 0; Li < Left.size(); ++Li) {
                        Superfetch::AdvanceJoinInner(Left, Li, FullJoinScan.Inner);
                        const auto &Lr = Left[Li];
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
            DbPtr->SetTableSchemaAssumeDbMutexHeld(*Dest, MergeJoinSchemas(LeftSch, RightSch));
            DbPtr->Tables_[*Dest] = std::move(Result);
            });
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
            switch (inst.Opcode_) {
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
            if(StackSlots_.size() < 2)
				FailVm("DATE operation requires two operands");
            const std::string Arg1 = PopOwnedStringMoved("DATE operation arg");
            const std::string Arg0 = PopOwnedStringMoved("DATE operation arg");
            Database::Item EmptyRow;
            const ScalarSqlFn Fn =
                inst.Opcode_ == Opcode::DATE_ADD ? ScalarSqlFn::DateAddDays : ScalarSqlFn::DateSubDays;
            const auto Got = EvalScalarSqlFn(Fn, {Arg0, Arg1}, EmptyRow, nullptr);
            PushOwningStringHeap(new std::string(Got.value_or(std::string())));
            ++Ic;
            break;
        }
        // JSON operations
        case Opcode::JSON_EXTRACT: {
            if(StackSlots_.size() < 2)
				FailVm("JSON_EXTRACT requires two operands");
            const std::string Path = PopOwnedStringMoved("JSON_EXTRACT path");
            const std::string Json = PopOwnedStringMoved("JSON_EXTRACT json");
			const auto Root = JsonSql::ParseCellJson(Json);
			if(!Root)
				PushOwningStringHeap(new std::string());
			else {
				const auto Got = JsonSql::ExtractPath(*Root, Path);
				PushOwningStringHeap(new std::string(Got ? JsonSql::JsonCellToText(*Got) : std::string{}));
			}
            ++Ic;
            break;
        }
		case Opcode::JSON_CONTAINS: {
			if(StackSlots_.size() < 2)
				FailVm("JSON_CONTAINS requires two operands");
			const std::string Needle = PopOwnedStringMoved("JSON_CONTAINS needle");
			const std::string Json = PopOwnedStringMoved("JSON_CONTAINS json");
			const auto Root = JsonSql::ParseCellJson(Json);
			bool Hit = false;
			if(Root) {
				const std::string Ser = JsonSql::JsonCellToText(*Root);
				Hit = Ser.find(Needle) != std::string::npos;
			}
			PushOwningStringHeap(new std::string(Hit ? "1" : "0"));
			++Ic;
			break;
		}
		case Opcode::JSON_MERGE: {
			if(StackSlots_.size() < 2)
				FailVm("JSON_MERGE requires two operands");
			const std::string B = PopOwnedStringMoved("JSON_MERGE b");
			const std::string A = PopOwnedStringMoved("JSON_MERGE a");
			const auto Ja = JsonSql::ParseCellJson(A);
			const auto Jb = JsonSql::ParseCellJson(B);
			if(!Ja || !Jb || !Ja->IsObject() || !Jb->IsObject())
				PushOwningStringHeap(new std::string(A));
			else {
				DS::JSONObject M = Ja->AsObject();
				for(const auto &[K, V] : Jb->AsObject())
					M[K] = V;
				PushOwningStringHeap(new std::string(DS::SerializeJSON(DS::JSON(std::move(M)))));
			}
			++Ic;
			break;
		}        case Opcode::REGEXP_MATCH: {
            if(StackSlots_.size() < 2)
                FailVm("REGEXP_MATCH requires two operands");
            const std::string Pat = PopOwnedStringMoved("REGEXP_MATCH pattern");
            const std::string Text = PopOwnedStringMoved("REGEXP_MATCH text");
            const bool Hit = SqlRegexpMatch(Text, Pat, false);
            PushOwningStringHeap(new std::string(Hit ? "1" : "0"));
            ++Ic;
            break;
        }
        // Full-text search
        case Opcode::MATCH:
        case Opcode::AGAINST: {
            if(StackSlots_.size() < 2)
				FailVm("MATCH/AGAINST requires two operands");
            const std::string Query = PopOwnedStringMoved("MATCH query");
            const std::string Text = PopOwnedStringMoved("MATCH text");
			const bool Hit = inst.Opcode_ == Opcode::AGAINST ? TextSearch::MatchAgainst(Text, Query) :
			                                                  TextSearch::MatchesQuery(Text, Query);
			PushOwningStringHeap(new std::string(Hit ? "1" : "0"));
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
            const bool Cube = (inst.Opcode_ == Opcode::CUBE);
            DbOlap->WithExclusiveBytecodeLock([&]() {
                auto &Tbl = DbOlap->Tables_[TableName].RowStore;
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
                auto &Tbl = DbGs->Tables_[TableName].RowStore;
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
                        ItSq == Databases_[0]->Tables_.end() ? kEmptySubquery : ItSq->second.RowStore;
                    switch(inst.Opcode_) {
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
                            result = (inst.Opcode_ == Opcode::ANY) ? anyMatch : allMatch;
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
		case Opcode::PROC_TRY: {
			if(inst.Operands.size() < 2)
				FailVm("PROC_TRY requires savepoint, end IP, and handler pairs");
			const auto *SavepointName = std::get_if<std::string>(&inst.Operands[0]);
			const auto *EndIp = std::get_if<int64_t>(&inst.Operands[1]);
			if(!SavepointName || !EndIp)
				FailVm("PROC_TRY expects string savepoint and int64 end IP");
			ProcTryFrame Frame;
			Frame.Savepoint = *SavepointName;
			Frame.EndIc = static_cast<std::size_t>(*EndIp);
			for(std::size_t O = 2; O + 1 < inst.Operands.size(); O += 2) {
				const auto *HandlerIp = std::get_if<int64_t>(&inst.Operands[O]);
				const auto *Cond = std::get_if<std::string>(&inst.Operands[O + 1]);
				if(!HandlerIp || !Cond)
					FailVm("PROC_TRY handler entries require int64 IP and string condition");
				Frame.Handlers.emplace_back(static_cast<std::size_t>(*HandlerIp), *Cond);
			}
			VmSavepoint(*SavepointName);
			ProcTryStack_.push_back(std::move(Frame));
			++Ic;
			break;
		}
		case Opcode::PROC_END_TRY: {
			if(inst.Operands.size() < 2)
				FailVm("PROC_END_TRY requires savepoint and end IP");
			const auto *SavepointName = std::get_if<std::string>(&inst.Operands[0]);
			const auto *EndIp = std::get_if<int64_t>(&inst.Operands[1]);
			if(!SavepointName || !EndIp)
				FailVm("PROC_END_TRY expects string savepoint and int64 end IP");
			VmReleaseSavepoint(*SavepointName);
			if(!ProcTryStack_.empty() && ProcTryStack_.back().Savepoint == *SavepointName)
				ProcTryStack_.pop_back();
			Ic = static_cast<uintptr_t>(*EndIp);
			break;
		}
		case Opcode::PROC_JUMP_IF_TABLE_EMPTY: {
			if(inst.Operands.size() < 2)
				FailVm("PROC_JUMP_IF_TABLE_EMPTY requires table name and jump IP");
			const auto *TableName = std::get_if<std::string>(&inst.Operands[0]);
			const auto *Target = std::get_if<int64_t>(&inst.Operands[1]);
			if(!TableName || !Target)
				FailVm("PROC_JUMP_IF_TABLE_EMPTY expects string table and int64 IP");
			if(*Target < 0 || static_cast<std::size_t>(*Target) >= Code.size())
				FailVm("PROC_JUMP_IF_TABLE_EMPTY target out of range");
			bool Empty = true;
			if(Databases_.empty())
				Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
			Databases_[0]->WithExclusiveBytecodeLock([&]() {
				const auto It = Databases_[0]->Tables_.find(*TableName);
				Empty = It == Databases_[0]->Tables_.end() || It->second.RowStore.empty();
				if(It != Databases_[0]->Tables_.end())
					Databases_[0]->Tables_.erase(It);
			});
			if(Empty)
				Ic = static_cast<uintptr_t>(*Target);
			else
				++Ic;
			break;
		}
        case Opcode::SAVEPOINT: {
            if (inst.Operands.empty()) FailVm("SAVEPOINT requires name");
            if (auto name = std::get_if<std::string>(&inst.Operands[0])) {
				VmSavepoint(*name);
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
				VmRollbackToSavepoint(*name);
                if(Logger_) Logger_->Info("Rolled back to SAVEPOINT \"" + *name + "\"");
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
				VmReleaseSavepoint(*Name);
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
            std::cerr << "Unimplemented opcode: " << static_cast<int>(inst.Opcode_) << std::endl;
            FailVm("Unimplemented opcode");
    }
    return true;
}

} // namespace SQL

bool EvaluatePackedWhereDnf(const Database *Db, const std::unordered_map<std::string, std::string> &Row,
                            std::string_view PackedDnfBlob) {
	return SQL::EvaluatePackedWhereDnf(Db, Row, PackedDnfBlob);
}

} // namespace AstralDB

