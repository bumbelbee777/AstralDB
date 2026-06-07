#include <SQL/Bulk/BulkOpcodePass.hxx>

#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <SQL/SQL.hxx>

#include <string>
#include <unordered_set>
#include <vector>

#include <algorithm>

namespace AstralDB {
namespace SQL {

namespace {

std::string NextScratch(std::size_t &Counter) {
	return "__astral_bulk_q_" + std::to_string(Counter++);
}

bool IsScratchTableName(std::string_view Name);
std::string ResolveBulkDimensionTable(const Bytecode &Code, std::size_t BeforeIx, std::string_view WorkName,
                                      const std::unordered_set<std::string> &BulkTables);

const std::string *StringOperand(const Instruction &Inst, std::size_t Index) {
	if(Index >= Inst.Operands.size())
		return nullptr;
	return std::get_if<std::string>(&Inst.Operands[Index]);
}

const std::string *PushTableName(const Instruction &Inst) {
	if(Inst.Opcode_ != Opcode::PUSH || Inst.Operands.size() != 1)
		return nullptr;
	const auto *S = std::get_if<std::string>(&Inst.Operands[0]);
	if(!S || S->empty())
		return nullptr;
	return S;
}

bool IsSelectColumnPush(const Instruction &Inst) {
	if(Inst.Opcode_ != Opcode::SELECT || Inst.Operands.empty())
		return false;
	return std::get_if<std::string>(&Inst.Operands[0]) != nullptr;
}

void AppendSelectColumnPushes(const Bytecode &Code, std::size_t From, std::size_t To,
                              std::vector<Instruction> &Out) {
	for(std::size_t S = From; S < To; ++S) {
		if(IsSelectColumnPush(Code[S]))
			Out.push_back(Code[S]);
	}
}

bool IsIntPush(const Instruction &Inst) {
	if(Inst.Opcode_ != Opcode::PUSH || Inst.Operands.size() != 1)
		return false;
	return std::get_if<int64_t>(&Inst.Operands[0]) != nullptr;
}

bool IsScalarCountGroupBy(const Instruction &Inst, std::string &OutCol) {
	if(Inst.Opcode_ != Opcode::GROUP_BY || Inst.Operands.size() < 2)
		return false;
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || *Tag != 1 || *Nk != 0)
		return false;
	if(Inst.Operands.size() >= 3) {
		if(const auto *C = std::get_if<std::string>(&Inst.Operands[2])) {
			OutCol = *C;
			return !OutCol.empty();
		}
	}
	OutCol = "cnt";
	return true;
}

bool IsJoinCountOnly(const Instruction &Inst, std::string &Dest, std::string &Left, std::string &Right,
                     std::string &LeftCol, std::string &RightCol) {
	if(Inst.Opcode_ != Opcode::INNER_JOIN || Inst.Operands.size() < 7)
		return false;
	const auto *D = StringOperand(Inst, 0);
	const auto *L = StringOperand(Inst, 1);
	const auto *R = StringOperand(Inst, 2);
	const auto *Lc = StringOperand(Inst, 4);
	const auto *Rc = StringOperand(Inst, 5);
	const auto *Flag = std::get_if<int64_t>(&Inst.Operands[6]);
	if(!D || !L || !R || !Lc || !Rc || !Flag || *Flag != 1)
		return false;
	Dest = *D;
	Left = *L;
	Right = *R;
	LeftCol = *Lc;
	RightCol = *Rc;
	return true;
}

bool ReadFilterDnf(const Instruction &Inst, std::vector<Value> &FilterTail) {
	if(Inst.Opcode_ != Opcode::FILTER_DNF || Inst.Operands.size() < 3)
		return false;
	const auto *BranchCount = std::get_if<int64_t>(&Inst.Operands[0]);
	if(!BranchCount || *BranchCount != 1)
		return false;
	const auto *PredCount = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!PredCount || *PredCount <= 0)
		return false;
	FilterTail.clear();
	FilterTail.push_back(*PredCount);
	for(int64_t P = 0; P < *PredCount; ++P) {
		const std::size_t Base = static_cast<std::size_t>(2 + P * 3);
		if(Base + 2 >= Inst.Operands.size())
			return false;
		FilterTail.push_back(Inst.Operands[Base]);
		FilterTail.push_back(Inst.Operands[Base + 1]);
		FilterTail.push_back(Inst.Operands[Base + 2]);
	}
	return true;
}

bool IsStarCubeShape(const Instruction &Inst, std::vector<std::string> &Keys, std::string &SumCol,
                     std::string &SumOut, std::string &AvgOut, std::string &CountOut) {
	if(Inst.Opcode_ != Opcode::CUBE || Inst.Operands.size() < 14)
		return false;
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3 || *Nk != 4)
		return false;
	Keys.clear();
	for(int64_t I = 0; I < 4; ++I) {
		const auto *K = StringOperand(Inst, static_cast<std::size_t>(2 + I));
		if(!K)
			return false;
		Keys.push_back(*K);
	}
	const size_t Base = static_cast<size_t>(2 + *Nk);
	if(Inst.Operands.size() < Base + 4)
		return false;
	const auto *AggCountFlag = std::get_if<int64_t>(&Inst.Operands[Base]);
	const auto *NumAggs = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
	if(!AggCountFlag || !NumAggs || *NumAggs < 1)
		return false;
	std::size_t Cursor = Base + 2;
	bool FoundSum = false;
	bool FoundAvg = false;
	for(int64_t A = 0; A < *NumAggs; ++A) {
		if(Cursor + 2 >= Inst.Operands.size())
			return false;
		const auto *Kind = std::get_if<int64_t>(&Inst.Operands[Cursor]);
		const auto *Src = StringOperand(Inst, Cursor + 1);
		const auto *Out = StringOperand(Inst, Cursor + 2);
		if(!Kind || !Src || !Out)
			return false;
		if(*Kind == static_cast<int64_t>(GroupCombAggKind::Sum)) {
			SumCol = *Src;
			SumOut = *Out;
			FoundSum = true;
		} else if(*Kind == static_cast<int64_t>(GroupCombAggKind::Avg)) {
			AvgOut = *Out;
			FoundAvg = true;
		}
		Cursor += 3;
	}
	if(*AggCountFlag) {
		const auto *Co = StringOperand(Inst, Cursor);
		if(!Co)
			return false;
		CountOut = *Co;
	}
	return FoundSum && FoundAvg && !CountOut.empty() && !SumCol.empty() && !SumOut.empty() && !AvgOut.empty();
}

bool ParseHavingCountMin(const Instruction &Inst, int64_t &OutMin) {
	if(Inst.Opcode_ != Opcode::FILTER_DNF || Inst.Operands.size() < 5)
		return false;
	const auto *Branches = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Preds = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Branches || *Branches != 1 || !Preds || *Preds != 1)
		return false;
	const auto *Col = StringOperand(Inst, 2);
	const auto *Op = StringOperand(Inst, 3);
	if(!Col || !Op)
		return false;
	if(*Op != ">" && *Op != ">=")
		return false;
	int64_t Threshold = 0;
	if(const auto *Lit = std::get_if<int64_t>(&Inst.Operands[4]))
		Threshold = *Lit;
	else if(const auto *LitS = StringOperand(Inst, 4)) {
		try {
			Threshold = std::stoll(*LitS);
		} catch(...) {
			return false;
		}
	} else
		return false;
	OutMin = (*Op == ">") ? Threshold + 1 : Threshold;
	return true;
}

bool IsStarGroupByShape(const Instruction &Inst, std::vector<std::string> &Keys, std::string &SumCol,
                        std::string &SumOut, std::string &CountOut) {
	if(Inst.Opcode_ != Opcode::GROUP_BY || Inst.Operands.size() < 10)
		return false;
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3 || *Nk != 2)
		return false;
	const auto *K0 = StringOperand(Inst, 2);
	const auto *K1 = StringOperand(Inst, 3);
	const auto *Na = std::get_if<int64_t>(&Inst.Operands[5]);
	if(!K0 || !K1 || !Na || *Na != 1)
		return false;
	const auto *Sc = StringOperand(Inst, 7);
	const auto *So = StringOperand(Inst, 8);
	const auto *Co = StringOperand(Inst, 9);
	if(!Sc || !So || !Co)
		return false;
	Keys = {*K0, *K1};
	SumCol = *Sc;
	SumOut = *So;
	CountOut = *Co;
	return true;
}

bool IsBulkEquiJoinPair(const Instruction &Inst, std::string &Dest, std::string &Left, std::string &Right,
                        std::string &LeftCol, std::string &RightCol) {
	if(Inst.Opcode_ != Opcode::INNER_JOIN || Inst.Operands.size() < 6)
		return false;
	const auto *D = StringOperand(Inst, 0);
	const auto *L = StringOperand(Inst, 1);
	const auto *R = StringOperand(Inst, 2);
	const auto *Lc = StringOperand(Inst, 4);
	const auto *Rc = StringOperand(Inst, 5);
	if(!D || !L || !R || !Lc || !Rc)
		return false;
	Dest = *D;
	Left = *L;
	Right = *R;
	LeftCol = *Lc;
	RightCol = *Rc;
	return true;
}

void ReplaceRange(Bytecode &Code, std::size_t Begin, std::size_t End, std::vector<Instruction> &&New) {
	Code.erase(Code.begin() + static_cast<std::ptrdiff_t>(Begin),
	           Code.begin() + static_cast<std::ptrdiff_t>(End));
	Code.insert(Code.begin() + static_cast<std::ptrdiff_t>(Begin), std::make_move_iterator(New.begin()),
	            std::make_move_iterator(New.end()));
}

bool TryLowerCountBulk(Bytecode &Code, const std::unordered_set<std::string> &BulkTables, std::size_t &Scratch) {
	bool Modified = false;
	for(std::size_t I = 0; I + 1 < Code.size(); ++I) {
		const std::string *Table = PushTableName(Code[I]);
		if(!Table)
			continue;
		std::string CountCol;
		if(!IsScalarCountGroupBy(Code[I + 1], CountCol))
			continue;
		if(BulkTables.find(*Table) == BulkTables.end())
			continue;
		const std::string Dest = NextScratch(Scratch);
		std::vector<Instruction> Repl;
		Repl.push_back(MakeInstruction(Opcode::COUNT_BULK, Dest, *Table, CountCol));
		ReplaceRange(Code, I, I + 2, std::move(Repl));
		Modified = true;
	}
	return Modified;
}

bool TryLowerJoinCountBulk(Bytecode &Code, const std::unordered_set<std::string> &BulkTables) {
	bool Modified = false;
	for(std::size_t I = 0; I + 3 < Code.size(); ++I) {
		std::string Dest, Left, Right, LeftCol, RightCol;
		if(!IsJoinCountOnly(Code[I], Dest, Left, Right, LeftCol, RightCol))
			continue;
		if(BulkTables.find(Left) == BulkTables.end() || BulkTables.find(Right) == BulkTables.end())
			continue;
		std::string CountCol;
		const std::size_t SelectBegin = I + 1;
		std::size_t PushIx = I + 1;
		if(PushIx < Code.size() && IsSelectColumnPush(Code[PushIx]))
			++PushIx;
		if(PushIx + 1 >= Code.size())
			continue;
		const std::string *PushName = PushTableName(Code[PushIx]);
		if(!PushName || *PushName != Dest)
			continue;
		if(!IsScalarCountGroupBy(Code[PushIx + 1], CountCol))
			continue;
		std::vector<Instruction> Repl;
		AppendSelectColumnPushes(Code, SelectBegin, PushIx, Repl);
		Repl.push_back(MakeInstruction(Opcode::JOIN_COUNT_BULK, Dest, Left, Right, LeftCol, RightCol, CountCol));
		ReplaceRange(Code, I, PushIx + 2, std::move(Repl));
		Modified = true;
	}
	return Modified;
}

bool TryLowerFilterCountBulk(Bytecode &Code, const std::unordered_set<std::string> &BulkTables, std::size_t &Scratch) {
	bool Modified = false;
	for(std::size_t I = 0; I + 2 < Code.size(); ++I) {
		const std::string *Table = PushTableName(Code[I]);
		if(!Table)
			continue;
		std::vector<Value> FilterTail;
		if(!ReadFilterDnf(Code[I + 1], FilterTail))
			continue;
		std::string CountCol;
		if(!IsScalarCountGroupBy(Code[I + 2], CountCol))
			continue;
		if(BulkTables.find(*Table) == BulkTables.end())
			continue;
		const std::string Dest = NextScratch(Scratch);
		std::vector<Instruction> Repl;
		Instruction Fc;
		Fc.Opcode_ = Opcode::FILTER_COUNT_BULK;
		Fc.Operands.push_back(Dest);
		Fc.Operands.push_back(*Table);
		for(Value &V : FilterTail)
			Fc.Operands.push_back(std::move(V));
		Fc.Operands.push_back(CountCol);
		Repl.push_back(std::move(Fc));
		ReplaceRange(Code, I, I + 3, std::move(Repl));
		Modified = true;
	}
	return Modified;
}

bool ParseScalarFuncEvalSpec(const Instruction &Inst, SemistructuredProjectionSpec &Out) {
	if(Inst.Opcode_ != Opcode::SCALAR_FUNC_EVAL || Inst.Operands.size() < 3)
		return false;
	const auto *OutCol = std::get_if<std::string>(&Inst.Operands[0]);
	const auto *FnTag = std::get_if<int64_t>(&Inst.Operands[1]);
	const auto *Argc = std::get_if<int64_t>(&Inst.Operands[2]);
	if(!OutCol || !FnTag || !Argc || *Argc < 0)
		return false;
	const std::size_t Need = 3 + static_cast<std::size_t>(*Argc) * 2;
	if(Inst.Operands.size() != Need)
		return false;
	Out.OutCol = *OutCol;
	Out.FnTag = static_cast<int>(*FnTag);
	Out.Args.clear();
	std::size_t Idx = 3;
	for(int64_t A = 0; A < *Argc; ++A) {
		const auto *Kind = std::get_if<int64_t>(&Inst.Operands[Idx++]);
		const auto *Pay = std::get_if<std::string>(&Inst.Operands[Idx++]);
		if(!Kind || !Pay)
			return false;
		Out.Args.emplace_back(*Kind, *Pay);
	}
	return true;
}

bool TryLowerSemistructuredTopkBulk(Bytecode &Code, const std::unordered_set<std::string> &BulkTables) {
	bool Modified = false;
	for(std::size_t I = 0; I + 2 < Code.size(); ++I) {
		const std::string *Table = PushTableName(Code[I]);
		if(!Table)
			continue;
		if(!BulkTables.empty() && BulkTables.find(*Table) == BulkTables.end()) {
			const std::string Resolved = ResolveBulkDimensionTable(Code, I, *Table, BulkTables);
			if(BulkTables.find(Resolved) == BulkTables.end())
				continue;
		}
		std::size_t J = I + 1;
		while(J < Code.size() && IsSelectColumnPush(Code[J]))
			++J;
		std::vector<SemistructuredProjectionSpec> Projections;
		while(J < Code.size()) {
			if(Code[J].Opcode_ == Opcode::SCALAR_FUNC_EVAL) {
				SemistructuredProjectionSpec Spec;
				if(!ParseScalarFuncEvalSpec(Code[J], Spec))
					break;
				Projections.push_back(std::move(Spec));
				++J;
				continue;
			}
			if(Code[J].Opcode_ == Opcode::CASE_EVAL) {
				++J;
				continue;
			}
			break;
		}
		if(J >= Code.size() || Code[J].Opcode_ != Opcode::FILTER_DNF)
			continue;
		std::vector<Value> FilterTail;
		if(!ReadFilterDnf(Code[J], FilterTail))
			continue;
		++J;
		while(J < Code.size()) {
			if(Code[J].Opcode_ == Opcode::SCALAR_FUNC_EVAL) {
				SemistructuredProjectionSpec Spec;
				if(!ParseScalarFuncEvalSpec(Code[J], Spec))
					break;
				Projections.push_back(std::move(Spec));
				++J;
				continue;
			}
			if(Code[J].Opcode_ == Opcode::CASE_EVAL) {
				++J;
				continue;
			}
			break;
		}
		if(Projections.empty() || J + 3 > Code.size())
			continue;
		if(!IsIntPush(Code[J]) || !IsIntPush(Code[J + 1]) || Code[J + 2].Opcode_ != Opcode::ORDER_BY)
			continue;
		const auto *Asc = std::get_if<int64_t>(&Code[J].Operands[0]);
		const auto *OrderCol = StringOperand(Code[J + 2], 0);
		if(!Asc || !OrderCol)
			continue;
		J += 3;
		if(J >= Code.size() || Code[J].Opcode_ != Opcode::LIMIT)
			continue;
		const auto *Lim = std::get_if<int64_t>(&Code[J].Operands[0]);
		if(!Lim || *Lim < 0)
			continue;
		++J;

		Instruction Fused;
		Fused.Opcode_ = Opcode::SEMISTRUCTURED_TOPK_BULK;
		Fused.Operands.push_back(*Table);
		for(Value &V : FilterTail)
			Fused.Operands.push_back(std::move(V));
		Fused.Operands.push_back(static_cast<int64_t>(Projections.size()));
		for(const SemistructuredProjectionSpec &P : Projections) {
			Fused.Operands.push_back(P.OutCol);
			Fused.Operands.push_back(static_cast<int64_t>(P.FnTag));
			Fused.Operands.push_back(static_cast<int64_t>(P.Args.size()));
			for(const auto &[Kind, Pay] : P.Args) {
				Fused.Operands.push_back(Kind);
				Fused.Operands.push_back(Pay);
			}
		}
		Fused.Operands.push_back(*OrderCol);
		Fused.Operands.push_back(*Asc);
		Fused.Operands.push_back(*Lim);
		std::vector<Instruction> Repl;
		Repl.push_back(std::move(Fused));
		ReplaceRange(Code, I + 1, J, std::move(Repl));
		Modified = true;
	}
	return Modified;
}

bool TryLowerStarGroupByBulk(Bytecode &Code) {
	bool Modified = false;
	for(std::size_t I = 0; I + 6 < Code.size(); ++I) {
		std::string Join1Dest, Join1Left, Join1Right, Join1Lc, Join1Rc;
		std::string Join2Dest, Join2Left, Join2Right, Join2Lc, Join2Rc;
		if(!IsBulkEquiJoinPair(Code[I], Join1Dest, Join1Left, Join1Right, Join1Lc, Join1Rc))
			continue;
		if(!IsBulkEquiJoinPair(Code[I + 1], Join2Dest, Join2Left, Join2Right, Join2Lc, Join2Rc))
			continue;
		if(Join2Left != Join1Dest)
			continue;
		const std::size_t SelectBegin = I + 2;
		std::size_t Cursor = I + 2;
		while(Cursor < Code.size() && IsSelectColumnPush(Code[Cursor]))
			++Cursor;
		if(Cursor >= Code.size())
			continue;
		std::string WorkTable = Join2Dest;
		std::size_t TableIx = Cursor;
		if(TableIx < Code.size() && Code[TableIx].Opcode_ == Opcode::CLONE_TABLE) {
			const auto *CloneDest = StringOperand(Code[TableIx], 0);
			const auto *CloneSrc = StringOperand(Code[TableIx], 1);
			if(!CloneDest || !CloneSrc || *CloneSrc != Join2Dest)
				continue;
			WorkTable = *CloneDest;
			++TableIx;
		}
		const std::string *PushName = PushTableName(Code[TableIx]);
		if(!PushName || *PushName != WorkTable)
			continue;
		std::vector<Value> FilterTail;
		std::size_t GroupIx = TableIx + 1;
		if(GroupIx < Code.size() && Code[GroupIx].Opcode_ == Opcode::FILTER_DNF) {
			if(ReadFilterDnf(Code[GroupIx], FilterTail)) {
				++GroupIx;
			} else {
				FilterTail.clear();
			}
		}
		if(GroupIx >= Code.size())
			continue;
		std::vector<std::string> Keys;
		std::string SumCol, SumOut, CountOut;
		if(!IsStarGroupByShape(Code[GroupIx], Keys, SumCol, SumOut, CountOut))
			continue;

		std::size_t AfterGroup = GroupIx + 1;
		while(AfterGroup < Code.size() && Code[AfterGroup].Opcode_ == Opcode::CASE_EVAL)
			++AfterGroup;

		std::size_t Begin = I;
		if(FilterTail.empty() && I >= 2 && Code[I - 2].Opcode_ == Opcode::FILTER_DNF) {
			if(ReadFilterDnf(Code[I - 2], FilterTail)) {
				Begin = I - 2;
			} else {
				FilterTail.clear();
			}
		}
		if(Begin >= 1 && Code[Begin - 1].Opcode_ == Opcode::PUSH)
			Begin -= 1;
		if(Begin >= 1 && Code[Begin - 1].Opcode_ == Opcode::STORAGE_HINT)
			Begin -= 1;

		std::string OrderCol = SumOut;
		int64_t Limit = -1;
		std::size_t End = AfterGroup;
		while(End < Code.size()) {
			if(Code[End].Opcode_ == Opcode::CASE_EVAL || Code[End].Opcode_ == Opcode::WINDOW_ROW_NUMBER) {
				++End;
				continue;
			}
			if(IsIntPush(Code[End])) {
				++End;
				continue;
			}
			if(Code[End].Opcode_ == Opcode::ORDER_BY) {
				if(const auto *Oc = StringOperand(Code[End], 0))
					OrderCol = *Oc;
				++End;
				continue;
			}
			if(Code[End].Opcode_ == Opcode::OFFSET) {
				++End;
				continue;
			}
			if(Code[End].Opcode_ == Opcode::LIMIT) {
				if(const auto *L = std::get_if<int64_t>(&Code[End].Operands[0]))
					Limit = *L;
				++End;
				break;
			}
			if(Code[End].Opcode_ == Opcode::SLICE_RANGE && Code[End].Operands.size() >= 2) {
				if(const auto *L = std::get_if<int64_t>(&Code[End].Operands[1]))
					Limit = *L;
				++End;
				break;
			}
			if(Code[End].Opcode_ == Opcode::SELECT && !Code[End].Operands.empty() &&
			   std::get_if<int64_t>(&Code[End].Operands[0]) != nullptr) {
				++End;
				break;
			}
			break;
		}
		if(Limit < 0)
			continue;

		std::vector<Instruction> Repl;
		AppendSelectColumnPushes(Code, SelectBegin, Cursor, Repl);
		Instruction Star;
		Star.Opcode_ = Opcode::STAR_GROUP_BY_BULK;
		Star.Operands.push_back(WorkTable);
		Star.Operands.push_back(Join1Left);
		Star.Operands.push_back(Join1Right);
		Star.Operands.push_back(Join2Right);
		if(FilterTail.empty()) {
			Star.Operands.push_back(static_cast<int64_t>(0));
		} else {
			for(Value &V : FilterTail)
				Star.Operands.push_back(std::move(V));
		}
		Star.Operands.push_back(Keys[0]);
		Star.Operands.push_back(Keys[1]);
		Star.Operands.push_back(SumCol);
		Star.Operands.push_back(SumOut);
		Star.Operands.push_back(CountOut);
		Star.Operands.push_back(OrderCol);
		Star.Operands.push_back(Limit);
		Repl.push_back(std::move(Star));
		ReplaceRange(Code, Begin, End, std::move(Repl));
		Modified = true;
	}
	return Modified;
}

bool TryLowerStarJoinCubeBulk(Bytecode &Code) {
	bool Modified = false;
	for(std::size_t I = 0; I + 1 < Code.size(); ++I) {
		std::string Join1Dest, Join1Left, Join1Right, Join1Lc, Join1Rc;
		std::string Join2Dest, Join2Left, Join2Right, Join2Lc, Join2Rc;
		if(!IsBulkEquiJoinPair(Code[I], Join1Dest, Join1Left, Join1Right, Join1Lc, Join1Rc))
			continue;
		if(!IsBulkEquiJoinPair(Code[I + 1], Join2Dest, Join2Left, Join2Right, Join2Lc, Join2Rc))
			continue;
		if(Join2Left != Join1Dest)
			continue;
		std::string CustTable = Join1Left;
		for(std::size_t B = 0; B < I; ++B) {
			if(Code[B].Opcode_ != Opcode::CLONE_TABLE)
				continue;
			const auto *D = StringOperand(Code[B], 0);
			const auto *S = StringOperand(Code[B], 1);
			if(D && S && *D == Join1Left) {
				CustTable = *S;
				break;
			}
		}
		std::size_t Cursor = I + 2;
		std::string WorkTable = Join2Dest;
		if(Cursor < Code.size() && Code[Cursor].Opcode_ == Opcode::CLONE_TABLE) {
			const auto *CloneDest = StringOperand(Code[Cursor], 0);
			const auto *CloneSrc = StringOperand(Code[Cursor], 1);
			if(!CloneDest || !CloneSrc || *CloneSrc != Join2Dest)
				continue;
			WorkTable = *CloneDest;
			++Cursor;
		}
		const std::size_t SelectBegin = Cursor;
		while(Cursor < Code.size() && IsSelectColumnPush(Code[Cursor]))
			++Cursor;
		if(Cursor >= Code.size() || Code[Cursor].Opcode_ != Opcode::PUSH)
			continue;
		const std::string *PushName = PushTableName(Code[Cursor]);
		if(!PushName || *PushName != WorkTable)
			continue;
		std::vector<Value> FilterTail;
		const std::size_t GroupIx = Cursor + 1;
		if(GroupIx >= Code.size() || Code[GroupIx].Opcode_ != Opcode::CUBE)
			continue;
		std::vector<std::string> Keys;
		std::string SumCol, SumOut, AvgOut, CountOut;
		if(!IsStarCubeShape(Code[GroupIx], Keys, SumCol, SumOut, AvgOut, CountOut))
			continue;
		std::size_t AfterCube = GroupIx + 1;
		int64_t HavingMin = 0;
		if(AfterCube >= Code.size() || Code[AfterCube].Opcode_ != Opcode::FILTER_DNF ||
		   !ParseHavingCountMin(Code[AfterCube], HavingMin))
			continue;
		const std::size_t End = AfterCube + 1;
		std::size_t Begin = I;
		if(I >= 3 && Code[I - 3].Opcode_ == Opcode::PUSH && Code[I - 2].Opcode_ == Opcode::FILTER_DNF &&
		   Code[I - 1].Opcode_ == Opcode::POP) {
			std::vector<Value> PreFilter;
			if(ReadFilterDnf(Code[I - 2], PreFilter)) {
				Begin = I - 3;
				if(FilterTail.empty())
					FilterTail = std::move(PreFilter);
			}
		}
		if(Begin >= 1 && Code[Begin - 1].Opcode_ == Opcode::STORAGE_HINT)
			Begin -= 1;
		std::vector<Instruction> Repl;
		AppendSelectColumnPushes(Code, SelectBegin, Cursor, Repl);
		Instruction Star;
		Star.Opcode_ = Opcode::STAR_JOIN_CUBE_BULK;
		Star.Operands.push_back(WorkTable);
		Star.Operands.push_back(CustTable);
		Star.Operands.push_back(Join1Right);
		Star.Operands.push_back(Join2Right);
		if(FilterTail.empty())
			Star.Operands.push_back(static_cast<int64_t>(0));
		else
			for(Value &V : FilterTail)
				Star.Operands.push_back(std::move(V));
		for(const std::string &K : Keys)
			Star.Operands.push_back(K);
		Star.Operands.push_back(SumCol);
		Star.Operands.push_back(SumOut);
		Star.Operands.push_back(AvgOut);
		Star.Operands.push_back(CountOut);
		Star.Operands.push_back(HavingMin);
		Repl.push_back(std::move(Star));
		ReplaceRange(Code, Begin, End, std::move(Repl));
		Modified = true;
		break;
	}
	return Modified;
}

bool IsScratchTableName(std::string_view Name) {
	return Name.starts_with("__AstralJoin_") || Name.starts_with("__astral_cte_") ||
	       Name.starts_with("__astral_");
}

std::string ResolveBulkDimensionTable(const Bytecode &Code, std::size_t BeforeIx, std::string_view WorkName,
                                      const std::unordered_set<std::string> &BulkTables) {
	if(!IsScratchTableName(WorkName) && BulkTables.find(std::string(WorkName)) != BulkTables.end())
		return std::string(WorkName);
	for(std::size_t J = BeforeIx; J > 0; --J) {
		const Instruction &Inst = Code[J - 1];
		if(Inst.Opcode_ != Opcode::CLONE_TABLE || Inst.Operands.size() < 2)
			continue;
		const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
		const auto *Src = std::get_if<std::string>(&Inst.Operands[1]);
		if(!Dest || !Src || *Dest != WorkName)
			continue;
		return ResolveBulkDimensionTable(Code, J - 1, *Src, BulkTables);
	}
	return std::string(WorkName);
}

void CollectReferencedBulkTables(const Bytecode &Code, std::unordered_set<std::string> &BulkTables) {
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::INSERT_BULK) {
			if(const auto *T = StringOperand(Inst, 0))
				BulkTables.insert(*T);
			continue;
		}
		if(Inst.Opcode_ == Opcode::CLONE_TABLE && Inst.Operands.size() >= 2) {
			if(const auto *Src = std::get_if<std::string>(&Inst.Operands[1])) {
				if(!IsScratchTableName(*Src))
					BulkTables.insert(*Src);
			}
			continue;
		}
	}
	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Instruction &Inst = Code[I];
		if(Inst.Opcode_ != Opcode::INNER_JOIN && Inst.Opcode_ != Opcode::CROSS_JOIN)
			continue;
		if(Inst.Operands.size() < 3)
			continue;
		if(const auto *L = std::get_if<std::string>(&Inst.Operands[1])) {
			const std::string Resolved = ResolveBulkDimensionTable(Code, I, *L, BulkTables);
			if(!IsScratchTableName(Resolved))
				BulkTables.insert(Resolved);
		}
		if(const auto *R = std::get_if<std::string>(&Inst.Operands[2])) {
			const std::string Resolved = ResolveBulkDimensionTable(Code, I, *R, BulkTables);
			if(!IsScratchTableName(Resolved))
				BulkTables.insert(Resolved);
		}
	}
}

void ResolveStarJoinFactDim(const Bytecode &Code, const std::size_t JoinIx, const std::string &JoinLeft,
                            const std::string &JoinRight, const std::unordered_set<std::string> &BulkTables,
                            std::string &OutFact, std::string &OutDim) {
	const std::string LeftR = ResolveBulkDimensionTable(Code, JoinIx, JoinLeft, BulkTables);
	const std::string RightR = ResolveBulkDimensionTable(Code, JoinIx, JoinRight, BulkTables);
	if(IsScratchTableName(JoinLeft) && !IsScratchTableName(JoinRight)) {
		OutFact = LeftR;
		OutDim = RightR;
		return;
	}
	if(!IsScratchTableName(JoinLeft) && IsScratchTableName(JoinRight)) {
		OutFact = RightR;
		OutDim = LeftR;
		return;
	}
	if(BulkTables.find(RightR) != BulkTables.end() && BulkTables.find(LeftR) == BulkTables.end()) {
		OutFact = LeftR;
		OutDim = RightR;
		return;
	}
	if(BulkTables.find(LeftR) != BulkTables.end() && BulkTables.find(RightR) == BulkTables.end()) {
		OutFact = RightR;
		OutDim = LeftR;
		return;
	}
	OutFact = LeftR;
	OutDim = RightR;
}

bool IsLazyMultiGroupShape(const Instruction &Inst, std::vector<std::string> &Keys) {
	if(Inst.Opcode_ != Opcode::GROUP_BY || Inst.Operands.size() < 4)
		return false;
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3 || *Nk <= 0)
		return false;
	if(*Nk == 2) {
		std::string SumCol, SumOut, CountOut;
		if(IsStarGroupByShape(Inst, Keys, SumCol, SumOut, CountOut))
			return false;
	}
	Keys.clear();
	for(int64_t Ki = 0; Ki < *Nk; ++Ki) {
		const auto *K = StringOperand(Inst, static_cast<std::size_t>(2 + Ki));
		if(!K || K->empty())
			return false;
		Keys.push_back(*K);
	}
	const std::size_t Base = static_cast<std::size_t>(2 + *Nk);
	if(Inst.Operands.size() <= Base + 1)
		return false;
	const auto *Na = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
	return Na && *Na > 0;
}

bool ParseOrderByKeys(const Bytecode &Code, std::size_t &J, std::string &Col1, bool &Asc1, std::string &Col2,
                      bool &Asc2) {
	Col1.clear();
	Col2.clear();
	Asc1 = true;
	Asc2 = true;
	if(J + 3 > Code.size() || !IsIntPush(Code[J]) || !IsIntPush(Code[J + 1]) || Code[J + 2].Opcode_ != Opcode::ORDER_BY)
		return false;
	const auto *Asc = std::get_if<int64_t>(&Code[J].Operands[0]);
	const auto *OrdCol = StringOperand(Code[J + 2], 0);
	if(!Asc || !OrdCol)
		return false;
	Col1 = *OrdCol;
	Asc1 = *Asc != 0;
	J += 3;
	if(J + 3 <= Code.size() && IsIntPush(Code[J]) && IsIntPush(Code[J + 1]) && Code[J + 2].Opcode_ == Opcode::ORDER_BY) {
		const auto *Asc2v = std::get_if<int64_t>(&Code[J].Operands[0]);
		const auto *OrdCol2 = StringOperand(Code[J + 2], 0);
		if(Asc2v && OrdCol2) {
			Col2 = *OrdCol2;
			Asc2 = *Asc2v != 0;
			J += 3;
		}
	}
	return true;
}

void AppendProjectionSpecs(std::vector<Instruction> &Repl, const std::vector<SemistructuredProjectionSpec> &Projections) {
	Repl.back().Operands.push_back(static_cast<int64_t>(Projections.size()));
	for(const SemistructuredProjectionSpec &P : Projections) {
		Repl.back().Operands.push_back(P.OutCol);
		Repl.back().Operands.push_back(static_cast<int64_t>(P.FnTag));
		Repl.back().Operands.push_back(static_cast<int64_t>(P.Args.size()));
		for(const auto &[Kind, Pay] : P.Args) {
			Repl.back().Operands.push_back(Kind);
			Repl.back().Operands.push_back(Pay);
		}
	}
}

void AppendGroupInstOperands(std::vector<Instruction> &Repl, const Instruction &GroupInst) {
	Repl.back().Operands.push_back(static_cast<int64_t>(GroupInst.Operands.size()));
	for(const Value &V : GroupInst.Operands)
		Repl.back().Operands.push_back(V);
}

std::string StripTablePrefix(std::string_view Col) {
	const std::size_t Dot = Col.find('.');
	return Dot == std::string::npos ? std::string(Col) : std::string(Col.substr(Dot + 1));
}

void RewriteBboxHelperInFilter(std::vector<Value> &FilterTail, const std::string &HelperCol,
                               std::string_view FactCol, std::string_view BboxRhs) {
	if(FilterTail.empty())
		return;
	const auto *PredCount = std::get_if<int64_t>(&FilterTail[0]);
	if(!PredCount || *PredCount <= 0)
		return;
	std::vector<std::tuple<std::string, std::string, std::string>> Kept;
	for(int64_t P = 0; P < *PredCount; ++P) {
		const std::size_t Base = static_cast<std::size_t>(1 + P * 3);
		if(Base + 2 >= FilterTail.size())
			break;
		const auto *Col = std::get_if<std::string>(&FilterTail[Base]);
		const auto *Op = std::get_if<std::string>(&FilterTail[Base + 1]);
		const auto *Lit = std::get_if<std::string>(&FilterTail[Base + 2]);
		if(!Col || !Op || !Lit)
			continue;
		if(*Col == HelperCol)
			continue;
		Kept.emplace_back(*Col, *Op, *Lit);
	}
	Kept.emplace_back(std::string(FactCol), std::string("__ST_BBOX__"), std::string(BboxRhs));
	FilterTail.clear();
	FilterTail.push_back(static_cast<int64_t>(Kept.size()));
	for(const auto &[Col, Op, Lit] : Kept) {
		FilterTail.push_back(Col);
		FilterTail.push_back(Op);
		FilterTail.push_back(Lit);
	}
}

bool ParseCaseEvalColumnCopy(const Instruction &Inst, std::string &SrcCol) {
	if(Inst.Opcode_ != Opcode::CASE_EVAL || Inst.Operands.size() < 4)
		return false;
	const auto *WhenCount = std::get_if<int64_t>(&Inst.Operands[1]);
	const auto *ElseKind = std::get_if<int64_t>(&Inst.Operands[2]);
	if(!WhenCount || !ElseKind || *WhenCount != 0 || *ElseKind != 1)
		return false;
	const auto *Src = std::get_if<std::string>(&Inst.Operands[3]);
	if(!Src)
		return false;
	SrcCol = StripTablePrefix(*Src);
	return true;
}

bool IsSupportedProjectionFn(const int FnTag) noexcept {
	switch(static_cast<ScalarSqlFn>(FnTag)) {
	case ScalarSqlFn::JsonExtract:
	case ScalarSqlFn::XmlValid:
	case ScalarSqlFn::XmlExtract:
	case ScalarSqlFn::RegexpExtract:
	case ScalarSqlFn::CharLength:
	case ScalarSqlFn::TextRank:
	case ScalarSqlFn::TextMatch:
	case ScalarSqlFn::StDistanceSpherical:
	case ScalarSqlFn::StWithinBbox:
	case ScalarSqlFn::TsDecompress:
		return true;
	default:
		return false;
	}
}

bool ParseCaseEvalTextMatch(const Instruction &Inst, SemistructuredProjectionSpec &Out,
                            const std::vector<std::string> &Passthrough) {
	if(Inst.Opcode_ != Opcode::CASE_EVAL || Inst.Operands.size() < 4)
		return false;
	const auto *OutCol = std::get_if<std::string>(&Inst.Operands[0]);
	const auto *WhenCount = std::get_if<int64_t>(&Inst.Operands[1]);
	const auto *ElseKind = std::get_if<int64_t>(&Inst.Operands[2]);
	const auto *Query = std::get_if<std::string>(&Inst.Operands[3]);
	if(!OutCol || !WhenCount || !ElseKind || !Query || *WhenCount != 0 || *ElseKind != 0)
		return false;
	std::string TextCol;
	for(const std::string &Col : Passthrough) {
		TextCol = Col;
		break;
	}
	if(TextCol.empty())
		return false;
	Out.OutCol = *OutCol;
	Out.FnTag = static_cast<int>(ScalarSqlFn::TextMatch);
	Out.Args = {{0, std::move(TextCol)}, {0, *Query}};
	return true;
}

bool TryLowerStarJoinSelectBulk(Bytecode &Code, const std::unordered_set<std::string> &BulkTables) {
	bool Modified = false;
	for(std::size_t I = 0; I + 1 < Code.size(); ++I) {
		std::string Join1Dest, Join1Left, Join1Right, Join1Lc, Join1Rc;
		std::string Join2Dest, Join2Left, Join2Right, Join2Lc, Join2Rc;
		if(!IsBulkEquiJoinPair(Code[I], Join1Dest, Join1Left, Join1Right, Join1Lc, Join1Rc))
			continue;
		if(!IsBulkEquiJoinPair(Code[I + 1], Join2Dest, Join2Left, Join2Right, Join2Lc, Join2Rc))
			continue;
		if(Join2Left != Join1Dest)
			continue;
		const std::string FactTable = Join1Left;
		if(BulkTables.find(FactTable) == BulkTables.end())
			continue;
		std::vector<Value> FilterTail;
		std::size_t Cursor = I + 2;
		std::vector<std::string> Passthrough;
		while(Cursor < Code.size() && IsSelectColumnPush(Code[Cursor]))
			++Cursor;
		if(Cursor >= Code.size())
			continue;
		std::string WorkTable = Join2Dest;
		if(Code[Cursor].Opcode_ == Opcode::CLONE_TABLE) {
			const auto *CloneDest = StringOperand(Code[Cursor], 0);
			const auto *CloneSrc = StringOperand(Code[Cursor], 1);
			if(!CloneDest || !CloneSrc || *CloneSrc != Join2Dest)
				continue;
			WorkTable = *CloneDest;
			++Cursor;
		}
		const std::string *PushName = PushTableName(Code[Cursor]);
		if(!PushName || *PushName != WorkTable)
			continue;
		++Cursor;
		SemistructuredProjectionSpec BboxHelper;
		bool HasBboxHelper = false;
		if(Cursor < Code.size() && Code[Cursor].Opcode_ == Opcode::SCALAR_FUNC_EVAL) {
			if(ParseScalarFuncEvalSpec(Code[Cursor], BboxHelper) &&
			   static_cast<ScalarSqlFn>(BboxHelper.FnTag) == ScalarSqlFn::StWithinBbox && BboxHelper.Args.size() >= 5) {
				HasBboxHelper = true;
				++Cursor;
			}
		}
		if(Cursor < Code.size() && Code[Cursor].Opcode_ == Opcode::FILTER_DNF) {
			std::vector<Value> PostFilter;
			if(ReadFilterDnf(Code[Cursor], PostFilter)) {
				if(FilterTail.empty())
					FilterTail = std::move(PostFilter);
				++Cursor;
			}
		}
		if(HasBboxHelper) {
			const std::string BboxRhs = BboxHelper.Args[1].second + "," + BboxHelper.Args[2].second + "," +
			                            BboxHelper.Args[3].second + "," + BboxHelper.Args[4].second;
			RewriteBboxHelperInFilter(FilterTail, BboxHelper.OutCol, StripTablePrefix(BboxHelper.Args[0].second),
			                          BboxRhs);
		}
		std::vector<SemistructuredProjectionSpec> Projections;
		while(Cursor < Code.size()) {
			if(Code[Cursor].Opcode_ == Opcode::SCALAR_FUNC_EVAL) {
				SemistructuredProjectionSpec Spec;
				if(!ParseScalarFuncEvalSpec(Code[Cursor], Spec)) {
					++Cursor;
					continue;
				}
				if(!IsSupportedProjectionFn(Spec.FnTag)) {
					++Cursor;
					continue;
				}
				Projections.push_back(std::move(Spec));
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::CASE_EVAL) {
				std::string SrcCol;
				if(ParseCaseEvalColumnCopy(Code[Cursor], SrcCol)) {
					if(std::find(Passthrough.begin(), Passthrough.end(), SrcCol) == Passthrough.end())
						Passthrough.push_back(SrcCol);
					++Cursor;
					continue;
				}
				SemistructuredProjectionSpec MatchSpec;
				if(ParseCaseEvalTextMatch(Code[Cursor], MatchSpec, Passthrough)) {
					Projections.push_back(std::move(MatchSpec));
					++Cursor;
					continue;
				}
				++Cursor;
				continue;
			}
			if(IsIntPush(Code[Cursor]))
				break;
			break;
		}
		if(Projections.empty())
			continue;
		std::string OrderCol1, OrderCol2;
		bool Asc1 = true, Asc2 = true;
		if(!ParseOrderByKeys(Code, Cursor, OrderCol1, Asc1, OrderCol2, Asc2))
			continue;
		if(Cursor >= Code.size() || Code[Cursor].Opcode_ != Opcode::LIMIT)
			continue;
		const auto *Lim = std::get_if<int64_t>(&Code[Cursor].Operands[0]);
		if(!Lim || *Lim <= 0)
			continue;
		++Cursor;
		std::size_t End = Cursor;
		while(End < Code.size() && (Code[End].Opcode_ == Opcode::SELECT || Code[End].Opcode_ == Opcode::CASE_EVAL))
			++End;

		std::size_t Begin = I;
		if(I >= 3 && Code[I - 3].Opcode_ == Opcode::PUSH && Code[I - 2].Opcode_ == Opcode::FILTER_DNF &&
		   Code[I - 1].Opcode_ == Opcode::POP) {
			std::vector<Value> PreFilter;
			if(ReadFilterDnf(Code[I - 2], PreFilter)) {
				Begin = I - 3;
				if(FilterTail.empty())
					FilterTail = std::move(PreFilter);
			}
		}
		if(Begin >= 1 && Code[Begin - 1].Opcode_ == Opcode::STORAGE_HINT)
			Begin -= 1;

		std::vector<Instruction> Repl;
		Instruction Star;
		Star.Opcode_ = Opcode::STAR_JOIN_SELECT_BULK;
		Star.Operands.push_back(WorkTable);
		Star.Operands.push_back(FactTable);
		Star.Operands.push_back(static_cast<int64_t>(2));
		Star.Operands.push_back(Join1Right);
		Star.Operands.push_back(Join2Right);
		if(FilterTail.empty())
			Star.Operands.push_back(static_cast<int64_t>(0));
		else
			for(Value &V : FilterTail)
				Star.Operands.push_back(std::move(V));
		Star.Operands.push_back(static_cast<int64_t>(Passthrough.size()));
		for(const std::string &Col : Passthrough)
			Star.Operands.push_back(Col);
		Repl.push_back(std::move(Star));
		AppendProjectionSpecs(Repl, Projections);
		Repl.back().Operands.push_back(OrderCol1);
		Repl.back().Operands.push_back(static_cast<int64_t>(Asc1 ? 1 : 0));
		Repl.back().Operands.push_back(OrderCol2);
		Repl.back().Operands.push_back(static_cast<int64_t>(Asc2 ? 1 : 0));
		Repl.back().Operands.push_back(*Lim);
		ReplaceRange(Code, Begin, End, std::move(Repl));
		Modified = true;
		break;
	}
	return Modified;
}

bool TryLowerFactDimGroupBulk(Bytecode &Code, const std::unordered_set<std::string> &BulkTables) {
	bool Modified = false;
	for(std::size_t I = 0; I + 1 < Code.size(); ++I) {
		std::string J1D, J1L, J1R, J1Lc, J1Rc;
		if(!IsBulkEquiJoinPair(Code[I], J1D, J1L, J1R, J1Lc, J1Rc))
			continue;
		std::string J2D, J2L, J2R, J2Lc, J2Rc;
		if(I + 1 < Code.size() && IsBulkEquiJoinPair(Code[I + 1], J2D, J2L, J2R, J2Lc, J2Rc) && J2L == J1D)
			continue;
		std::string FactTable;
		std::string Dim0;
		ResolveStarJoinFactDim(Code, I, J1L, J1R, BulkTables, FactTable, Dim0);
		if(BulkTables.find(FactTable) == BulkTables.end())
			continue;
		std::size_t Cursor = I + 1;
		std::string WorkTable = J1D;
		std::vector<Value> FilterTail;
		std::vector<SemistructuredProjectionSpec> ComputedScalars;
		std::size_t GroupIx = static_cast<std::size_t>(-1);
		while(Cursor < Code.size()) {
			if(Code[Cursor].Opcode_ == Opcode::CLONE_TABLE) {
				const auto *CloneDest = StringOperand(Code[Cursor], 0);
				const auto *CloneSrc = StringOperand(Code[Cursor], 1);
				if(CloneDest && CloneSrc)
					WorkTable = *CloneDest;
				++Cursor;
				continue;
			}
			if(IsSelectColumnPush(Code[Cursor])) {
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::SCALAR_FUNC_EVAL) {
				SemistructuredProjectionSpec Spec;
				if(!ParseScalarFuncEvalSpec(Code[Cursor], Spec))
					break;
				ComputedScalars.push_back(std::move(Spec));
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::FILTER_DNF) {
				std::vector<Value> Local;
				if(ReadFilterDnf(Code[Cursor], Local) && FilterTail.empty())
					FilterTail = std::move(Local);
				++Cursor;
				continue;
			}
			if(const std::string *PushName = PushTableName(Code[Cursor])) {
				WorkTable = *PushName;
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::GROUP_BY) {
				std::vector<std::string> Keys;
				if(IsLazyMultiGroupShape(Code[Cursor], Keys))
					GroupIx = Cursor;
				else {
					std::string SumCol, SumOut, CountOut;
					if(IsStarGroupByShape(Code[Cursor], Keys, SumCol, SumOut, CountOut))
						GroupIx = Cursor;
				}
				break;
			}
			if(Code[Cursor].Opcode_ == Opcode::NOP || Code[Cursor].Opcode_ == Opcode::STORAGE_HINT ||
			   Code[Cursor].Opcode_ == Opcode::CASE_EVAL)
				++Cursor;
			else if(Code[Cursor].Opcode_ == Opcode::SELECT && !IsSelectColumnPush(Code[Cursor]))
				++Cursor;
			else
				break;
		}
		if(GroupIx == static_cast<std::size_t>(-1))
			continue;
		const Instruction GroupInst = Code[GroupIx];
		Cursor = GroupIx + 1;
		while(Cursor < Code.size()) {
			if(Code[Cursor].Opcode_ == Opcode::SCALAR_FUNC_EVAL) {
				SemistructuredProjectionSpec Spec;
				if(!ParseScalarFuncEvalSpec(Code[Cursor], Spec))
					break;
				ComputedScalars.push_back(std::move(Spec));
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::CASE_EVAL) {
				++Cursor;
				continue;
			}
			break;
		}
		std::string OrderCol;
		bool OrderDesc = false;
		if(Cursor + 3 <= Code.size() && IsIntPush(Code[Cursor]) && IsIntPush(Code[Cursor + 1]) &&
		   Code[Cursor + 2].Opcode_ == Opcode::ORDER_BY) {
			const auto *Asc = std::get_if<int64_t>(&Code[Cursor].Operands[0]);
			if(const auto *Oc = StringOperand(Code[Cursor + 2], 0))
				OrderCol = *Oc;
			if(Asc)
				OrderDesc = *Asc == 0;
			Cursor += 3;
		}
		int64_t LimVal = 0;
		if(Cursor < Code.size() && Code[Cursor].Opcode_ == Opcode::LIMIT) {
			if(const auto *Lim = std::get_if<int64_t>(&Code[Cursor].Operands[0]))
				LimVal = *Lim;
			++Cursor;
		}
		while(Cursor < Code.size()) {
			if(Code[Cursor].Opcode_ == Opcode::CASE_EVAL || Code[Cursor].Opcode_ == Opcode::WINDOW_ROW_NUMBER) {
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::SELECT && !IsSelectColumnPush(Code[Cursor])) {
				++Cursor;
				break;
			}
			break;
		}
		const std::size_t End = Cursor;
		std::size_t Begin = I;
		if(I >= 3 && Code[I - 3].Opcode_ == Opcode::PUSH && Code[I - 2].Opcode_ == Opcode::FILTER_DNF &&
		   Code[I - 1].Opcode_ == Opcode::POP) {
			std::vector<Value> PreFilter;
			if(ReadFilterDnf(Code[I - 2], PreFilter)) {
				Begin = I - 3;
				if(FilterTail.empty())
					FilterTail = std::move(PreFilter);
			}
		}
		if(Begin >= 1 && Code[Begin - 1].Opcode_ == Opcode::STORAGE_HINT)
			Begin -= 1;
		std::vector<Instruction> Repl;
		Instruction Star;
		Star.Opcode_ = Opcode::STAR_JOIN_GROUP_BULK;
		Star.Operands.push_back(WorkTable);
		Star.Operands.push_back(FactTable);
		Star.Operands.push_back(static_cast<int64_t>(1));
		Star.Operands.push_back(Dim0);
		if(FilterTail.empty())
			Star.Operands.push_back(static_cast<int64_t>(0));
		else
			for(Value &V : FilterTail)
				Star.Operands.push_back(std::move(V));
		Repl.push_back(std::move(Star));
		AppendGroupInstOperands(Repl, GroupInst);
		AppendProjectionSpecs(Repl, ComputedScalars);
		Repl.back().Operands.push_back(OrderCol);
		Repl.back().Operands.push_back(static_cast<int64_t>(OrderDesc ? 1 : 0));
		Repl.back().Operands.push_back(LimVal);
		ReplaceRange(Code, Begin, End, std::move(Repl));
		Modified = true;
		break;
	}
	return Modified;
}

bool TryLowerStarJoinGroupBulk(Bytecode &Code, const std::unordered_set<std::string> &BulkTables) {
	bool Modified = false;
	for(std::size_t I = 0; I + 2 < Code.size(); ++I) {
		std::string J1D, J1L, J1R, J1Lc, J1Rc;
		std::string J2D, J2L, J2R, J2Lc, J2Rc;
		std::string J3D, J3L, J3R, J3Lc, J3Rc;
		if(!IsBulkEquiJoinPair(Code[I], J1D, J1L, J1R, J1Lc, J1Rc))
			continue;
		if(!IsBulkEquiJoinPair(Code[I + 1], J2D, J2L, J2R, J2Lc, J2Rc))
			continue;
		if(!IsBulkEquiJoinPair(Code[I + 2], J3D, J3L, J3R, J3Lc, J3Rc))
			continue;
		if(J2L != J1D || J3L != J2D)
			continue;
		const std::string FactTable = J1R;
		if(BulkTables.find(FactTable) == BulkTables.end())
			continue;
		const std::string Dim0 = ResolveBulkDimensionTable(Code, I, J1L, BulkTables);
		const std::vector<std::string> DimTables = {Dim0, J2R, J3R};
		std::size_t Cursor = I + 3;
		std::string WorkTable = J3D;
		std::vector<Value> FilterTail;
		std::vector<SemistructuredProjectionSpec> ComputedScalars;
		SemistructuredProjectionSpec BboxHelper;
		bool HasBboxHelper = false;
		std::size_t GroupIx = static_cast<std::size_t>(-1);
		while(Cursor < Code.size()) {
			if(Code[Cursor].Opcode_ == Opcode::CLONE_TABLE) {
				const auto *CloneDest = StringOperand(Code[Cursor], 0);
				const auto *CloneSrc = StringOperand(Code[Cursor], 1);
				if(CloneDest && CloneSrc)
					WorkTable = *CloneDest;
				++Cursor;
				continue;
			}
			if(IsSelectColumnPush(Code[Cursor])) {
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::SCALAR_FUNC_EVAL) {
				SemistructuredProjectionSpec Spec;
				if(!ParseScalarFuncEvalSpec(Code[Cursor], Spec))
					break;
				if(static_cast<ScalarSqlFn>(Spec.FnTag) == ScalarSqlFn::StWithinBbox && Spec.Args.size() >= 5) {
					BboxHelper = std::move(Spec);
					HasBboxHelper = true;
				} else {
					ComputedScalars.push_back(std::move(Spec));
				}
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::FILTER_DNF) {
				std::vector<Value> Local;
				if(ReadFilterDnf(Code[Cursor], Local)) {
					if(FilterTail.empty())
						FilterTail = std::move(Local);
					if(HasBboxHelper) {
						const std::string BboxRhs = BboxHelper.Args[1].second + "," + BboxHelper.Args[2].second + "," +
						                            BboxHelper.Args[3].second + "," + BboxHelper.Args[4].second;
						RewriteBboxHelperInFilter(FilterTail, BboxHelper.OutCol,
						                          StripTablePrefix(BboxHelper.Args[0].second), BboxRhs);
					}
				}
				++Cursor;
				continue;
			}
			if(const std::string *PushName = PushTableName(Code[Cursor])) {
				WorkTable = *PushName;
				++Cursor;
				continue;
			}
			if(Code[Cursor].Opcode_ == Opcode::GROUP_BY) {
				std::vector<std::string> Keys;
				if(IsLazyMultiGroupShape(Code[Cursor], Keys))
					GroupIx = Cursor;
				break;
			}
			if(Code[Cursor].Opcode_ == Opcode::NOP || Code[Cursor].Opcode_ == Opcode::STORAGE_HINT ||
			   Code[Cursor].Opcode_ == Opcode::CASE_EVAL)
				++Cursor;
			else if(Code[Cursor].Opcode_ == Opcode::SELECT && !IsSelectColumnPush(Code[Cursor]))
				++Cursor;
			else
				break;
		}
		if(GroupIx == static_cast<std::size_t>(-1))
			continue;
		Cursor = GroupIx + 1;
		const Instruction GroupInst = Code[GroupIx];
		std::string OrderCol;
		bool OrderDesc = false;
		if(Cursor + 3 <= Code.size() && IsIntPush(Code[Cursor]) && IsIntPush(Code[Cursor + 1]) &&
		   Code[Cursor + 2].Opcode_ == Opcode::ORDER_BY) {
			const auto *Asc = std::get_if<int64_t>(&Code[Cursor].Operands[0]);
			if(const auto *Oc = StringOperand(Code[Cursor + 2], 0))
				OrderCol = *Oc;
			if(Asc)
				OrderDesc = *Asc == 0;
			Cursor += 3;
		}
		if(Cursor >= Code.size() || Code[Cursor].Opcode_ != Opcode::LIMIT)
			continue;
		const auto *Lim = std::get_if<int64_t>(&Code[Cursor].Operands[0]);
		if(!Lim || *Lim <= 0)
			continue;
		++Cursor;
		std::size_t End = Cursor;
		while(End < Code.size() && (Code[End].Opcode_ == Opcode::SELECT || Code[End].Opcode_ == Opcode::CASE_EVAL))
			++End;

		std::size_t Begin = I;
		if(I >= 3 && Code[I - 3].Opcode_ == Opcode::PUSH && Code[I - 2].Opcode_ == Opcode::FILTER_DNF &&
		   Code[I - 1].Opcode_ == Opcode::POP) {
			std::vector<Value> PreFilter;
			if(ReadFilterDnf(Code[I - 2], PreFilter)) {
				Begin = I - 3;
				if(FilterTail.empty())
					FilterTail = std::move(PreFilter);
			}
		}
		if(Begin >= 1 && Code[Begin - 1].Opcode_ == Opcode::STORAGE_HINT)
			Begin -= 1;

		std::vector<Instruction> Repl;
		Instruction Star;
		Star.Opcode_ = Opcode::STAR_JOIN_GROUP_BULK;
		Star.Operands.push_back(WorkTable);
		Star.Operands.push_back(FactTable);
		Star.Operands.push_back(static_cast<int64_t>(DimTables.size()));
		for(const std::string &Dim : DimTables)
			Star.Operands.push_back(Dim);
		if(FilterTail.empty())
			Star.Operands.push_back(static_cast<int64_t>(0));
		else
			for(Value &V : FilterTail)
				Star.Operands.push_back(std::move(V));
		Repl.push_back(std::move(Star));
		AppendGroupInstOperands(Repl, GroupInst);
		AppendProjectionSpecs(Repl, ComputedScalars);
		Repl.back().Operands.push_back(OrderCol);
		Repl.back().Operands.push_back(static_cast<int64_t>(OrderDesc ? 1 : 0));
		Repl.back().Operands.push_back(*Lim);
		ReplaceRange(Code, Begin, End, std::move(Repl));
		Modified = true;
		break;
	}
	return Modified;
}

} // namespace

bool BulkOpcodePass::Run(Bytecode &Code, Logger *Logger) {
	(void)Logger;
	std::unordered_set<std::string> BulkTables;
	CollectReferencedBulkTables(Code, BulkTables);
	std::size_t Scratch = 0;

	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Instruction &Inst = Code[I];
		if(Inst.Opcode_ == Opcode::INNER_JOIN && Inst.Operands.size() >= 3) {
			if(const auto *L = StringOperand(Inst, 1))
				if(!IsScratchTableName(*L))
					BulkTables.insert(*L);
			if(const auto *R = StringOperand(Inst, 2))
				if(!IsScratchTableName(*R))
					BulkTables.insert(*R);
		}
	}

	bool Modified = false;
	Modified = TryLowerCountBulk(Code, BulkTables, Scratch) || Modified;
	Modified = TryLowerJoinCountBulk(Code, BulkTables) || Modified;
	Modified = TryLowerFilterCountBulk(Code, BulkTables, Scratch) || Modified;
	Modified = TryLowerStarGroupByBulk(Code) || Modified;
	Modified = TryLowerStarJoinCubeBulk(Code) || Modified;
	Modified = TryLowerStarJoinSelectBulk(Code, BulkTables) || Modified;
	Modified = TryLowerFactDimGroupBulk(Code, BulkTables) || Modified;
	Modified = TryLowerStarJoinGroupBulk(Code, BulkTables) || Modified;
	Modified = TryLowerSemistructuredTopkBulk(Code, BulkTables) || Modified;
	return Modified;
}

void RunBulkOpcodePass(Bytecode &Code, OptimizationLevel OptLevel, Logger *Logger) {
	if(OptLevel < OptimizationLevel::Advanced || Code.empty())
		return;
	BulkOpcodePass Pass;
	const int Rounds = OptLevel >= OptimizationLevel::Maximum ? 3 : 1;
	for(int Round = 0; Round < Rounds; ++Round) {
		if(!Pass.Run(Code, Logger))
			break;
	}
}

} // namespace SQL
} // namespace AstralDB
