#include <SQL/Bytecode/BytecodeInspect.hxx>
#include <SQL/Bytecode/BytecodeDisasm.hxx>
#include <SQL/Bytecode/OpcodeMeta.hxx>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace AstralDB {
namespace SQL {

namespace {

bool IsScratchTableName(std::string_view Name) {
	return Name.starts_with("__AstralJoin_") || Name.starts_with("__astral_cte_") ||
	       Name.starts_with("__astral_");
}

void MaybeAddUnique(std::vector<std::string> &Out, const std::string &Name) {
	if(Name.empty())
		return;
	if(std::find(Out.begin(), Out.end(), Name) != Out.end())
		return;
	Out.push_back(Name);
}

void MaybeAddTableRef(BytecodeAnalysis &Analysis, const std::string &Name) {
	if(Name.empty())
		return;
	if(IsScratchTableName(Name))
		MaybeAddUnique(Analysis.ScratchTables, Name);
	else
		MaybeAddUnique(Analysis.ReferencedTables, Name);
}

void HarvestOperands(const Instruction &Inst, BytecodeAnalysis &Analysis) {
	const auto FirstString = [&Inst]() -> const std::string * {
		for(const auto &Op : Inst.Operands)
			if(const auto *S = std::get_if<std::string>(&Op))
				return S;
		return nullptr;
	};
	switch(Inst.Opcode_) {
	case Opcode::CREATE_TABLE:
	case Opcode::DROP_TABLE:
	case Opcode::INSERT:
	case Opcode::INSERT_BULK:
	case Opcode::DELETE:
	case Opcode::DELETE_MATCHING:
	case Opcode::UPDATE_MATCHING:
	case Opcode::CLONE_TABLE:
	case Opcode::CREATE_VIEW:
	case Opcode::DROP_VIEW:
	case Opcode::ALTER_TABLE:
	case Opcode::RENAME_TABLE:
	case Opcode::CREATE_INDEX:
	case Opcode::DROP_INDEX:
	case Opcode::MERGE_INTO:
	case Opcode::UPSERT:
		if(const auto *S = FirstString())
			MaybeAddTableRef(Analysis, *S);
		break;
	case Opcode::INNER_JOIN:
	case Opcode::LEFT_JOIN:
	case Opcode::RIGHT_JOIN:
	case Opcode::FULL_JOIN:
	case Opcode::CROSS_JOIN:
		for(const auto &Op : Inst.Operands)
			if(const auto *S = std::get_if<std::string>(&Op))
				MaybeAddTableRef(Analysis, *S);
		break;
	case Opcode::CREATE_SEQUENCE:
	case Opcode::DROP_SEQUENCE:
		if(const auto *S = FirstString())
			MaybeAddUnique(Analysis.ReferencedSequences, *S);
		break;
	case Opcode::CREATE_ROLE:
	case Opcode::DROP_ROLE:
	case Opcode::GRANT_ROLE_MEMBERSHIP:
	case Opcode::REVOKE_ROLE_MEMBERSHIP:
		if(const auto *S = FirstString())
			MaybeAddUnique(Analysis.ReferencedRoles, *S);
		break;
	case Opcode::CALL_PROCEDURE:
		if(const auto *S = FirstString())
			MaybeAddUnique(Analysis.ReferencedProcedures, *S);
		break;
	case Opcode::PROC_TRY:
		Analysis.HasProcedureExceptionHandlers = true;
		for(std::size_t O = 3; O + 1 < Inst.Operands.size(); O += 2)
			if(const auto *C = std::get_if<std::string>(&Inst.Operands[O + 1]))
				MaybeAddUnique(Analysis.ExceptionConditions, *C);
		break;
	default:
		break;
	}
}

} // namespace

BytecodeAnalysis AnalyzeBytecode(const Bytecode &Code) {
	BytecodeAnalysis Analysis;
	Analysis.InstructionCount = Code.size();
	for(const Instruction &Inst : Code) {
		++Analysis.OpcodeHistogram[OpcodeName(Inst.Opcode_)];
		if(Inst.IsPure())
			++Analysis.PureInstructionCount;
		if(Inst.HasSideEffects())
			++Analysis.SideEffectInstructionCount;
		if(Inst.Opcode_ == Opcode::HALT)
			Analysis.HasHalt = true;
		if(OpcodeIsDdlMeta(Inst.Opcode_))
			Analysis.HasDdl = true;
		if(OpcodeIsDmlMeta(Inst.Opcode_))
			Analysis.HasDml = true;
		if(OpcodeIsJoinMeta(Inst.Opcode_))
			Analysis.HasJoins = true;
		if(OpcodeIsAggregateMeta(Inst.Opcode_))
			Analysis.HasAggregates = true;
		if(Inst.Opcode_ == Opcode::WINDOW_ROW_NUMBER)
			Analysis.HasWindowAnalytics = true;
		if(OpcodeIsSecurityMeta(Inst.Opcode_))
			Analysis.HasSecurityGrants = true;
		switch(Inst.Opcode_) {
		case Opcode::BEGIN:
		case Opcode::COMMIT:
		case Opcode::ROLLBACK:
		case Opcode::SAVEPOINT:
		case Opcode::ROLLBACK_TO:
		case Opcode::RELEASE_SAVEPOINT:
			Analysis.HasTransactionControl = true;
			break;
		default:
			break;
		}
		HarvestOperands(Inst, Analysis);
	}
	return Analysis;
}

std::string FormatBytecodeAnalysis(const BytecodeAnalysis &Analysis) {
	std::ostringstream Out;
	Out << "instructions=" << Analysis.InstructionCount << "\n";
	Out << "pure_ops=" << Analysis.PureInstructionCount << "\n";
	Out << "side_effect_ops=" << Analysis.SideEffectInstructionCount << "\n";
	Out << "has_halt=" << (Analysis.HasHalt ? "yes" : "no") << "\n";
	Out << "ddl=" << (Analysis.HasDdl ? "yes" : "no") << "\n";
	Out << "dml=" << (Analysis.HasDml ? "yes" : "no") << "\n";
	Out << "transactions=" << (Analysis.HasTransactionControl ? "yes" : "no") << "\n";
	Out << "joins=" << (Analysis.HasJoins ? "yes" : "no") << "\n";
	Out << "aggregates=" << (Analysis.HasAggregates ? "yes" : "no") << "\n";
	Out << "window_analytics=" << (Analysis.HasWindowAnalytics ? "yes" : "no") << "\n";
	Out << "security_grants=" << (Analysis.HasSecurityGrants ? "yes" : "no") << "\n";
	auto EmitList = [&Out](const char *Label, const std::vector<std::string> &Items) {
		Out << Label << "=";
		if(Items.empty()) {
			Out << "(none)\n";
			return;
		}
		for(std::size_t I = 0; I < Items.size(); ++I) {
			if(I)
				Out << ",";
			Out << Items[I];
		}
		Out << "\n";
	};
	EmitList("tables", Analysis.ReferencedTables);
	EmitList("scratch_tables", Analysis.ScratchTables);
	EmitList("sequences", Analysis.ReferencedSequences);
	EmitList("roles", Analysis.ReferencedRoles);
	EmitList("procedures", Analysis.ReferencedProcedures);
	EmitList("exception_conditions", Analysis.ExceptionConditions);
	Out << "procedure_exception_handlers=" << (Analysis.HasProcedureExceptionHandlers ? "yes" : "no") << "\n";
	Out << "opcode_histogram:\n";
	for(const auto &[Name, Count] : Analysis.OpcodeHistogram)
		Out << "  " << Name << " " << Count << "\n";
	return Out.str();
}

BytecodeAspectKind ParseBytecodeAspectKind(std::string_view AspectName) {
	std::string Lower(AspectName);
	std::transform(Lower.begin(), Lower.end(), Lower.begin(),
	               [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
	if(Lower == "stats" || Lower == "summary")
		return BytecodeAspectKind::Stats;
	if(Lower == "ddl")
		return BytecodeAspectKind::Ddl;
	if(Lower == "dml")
		return BytecodeAspectKind::Dml;
	if(Lower == "tables")
		return BytecodeAspectKind::Tables;
	if(Lower == "sequences")
		return BytecodeAspectKind::Sequences;
	if(Lower == "opcodes" || Lower == "histogram")
		return BytecodeAspectKind::Opcodes;
	if(Lower == "side-effects" || Lower == "side_effects" || Lower == "effects")
		return BytecodeAspectKind::SideEffects;
	if(Lower == "transactions" || Lower == "txn")
		return BytecodeAspectKind::Transactions;
	if(Lower == "joins")
		return BytecodeAspectKind::Joins;
	if(Lower == "aggregates" || Lower == "olap")
		return BytecodeAspectKind::Aggregates;
	if(Lower == "security" || Lower == "grants")
		return BytecodeAspectKind::Security;
	if(Lower == "all")
		return BytecodeAspectKind::All;
	throw std::runtime_error("Unknown bytecode aspect \"" + std::string(AspectName) +
	                         "\" (try stats, ddl, dml, tables, opcodes, side-effects, transactions, joins, "
	                         "aggregates, security, or all).");
}

std::string FormatBytecodeAspect(const BytecodeAnalysis &Analysis, BytecodeAspectKind Kind) {
	std::ostringstream Out;
	switch(Kind) {
	case BytecodeAspectKind::Stats:
		Out << "instructions=" << Analysis.InstructionCount << "\n";
		Out << "pure_ops=" << Analysis.PureInstructionCount << "\n";
		Out << "side_effect_ops=" << Analysis.SideEffectInstructionCount << "\n";
		Out << "has_halt=" << (Analysis.HasHalt ? "yes" : "no") << "\n";
		break;
	case BytecodeAspectKind::Ddl:
		Out << (Analysis.HasDdl ? "yes" : "no") << "\n";
		break;
	case BytecodeAspectKind::Dml:
		Out << (Analysis.HasDml ? "yes" : "no") << "\n";
		break;
	case BytecodeAspectKind::Tables:
		for(const auto &T : Analysis.ReferencedTables)
			Out << T << "\n";
		break;
	case BytecodeAspectKind::Sequences:
		for(const auto &S : Analysis.ReferencedSequences)
			Out << S << "\n";
		break;
	case BytecodeAspectKind::Opcodes:
		for(const auto &[Name, Count] : Analysis.OpcodeHistogram)
			Out << Name << "\t" << Count << "\n";
		break;
	case BytecodeAspectKind::SideEffects:
		Out << "count=" << Analysis.SideEffectInstructionCount << "\n";
		break;
	case BytecodeAspectKind::Transactions:
		Out << (Analysis.HasTransactionControl ? "yes" : "no") << "\n";
		break;
	case BytecodeAspectKind::Joins:
		Out << (Analysis.HasJoins ? "yes" : "no") << "\n";
		break;
	case BytecodeAspectKind::Aggregates:
		Out << (Analysis.HasAggregates ? "yes" : "no") << "\n";
		Out << "window=" << (Analysis.HasWindowAnalytics ? "yes" : "no") << "\n";
		break;
	case BytecodeAspectKind::Security:
		Out << (Analysis.HasSecurityGrants ? "yes" : "no") << "\n";
		for(const auto &R : Analysis.ReferencedRoles)
			Out << R << "\n";
		break;
	case BytecodeAspectKind::All:
		return FormatBytecodeAnalysis(Analysis);
	}
	return Out.str();
}

std::string QueryBytecodeAspect(const Bytecode &Code, std::string_view AspectName) {
	const auto Kind = ParseBytecodeAspectKind(AspectName);
	return FormatBytecodeAspect(AnalyzeBytecode(Code), Kind);
}

BytecodeValidationReport ValidateBytecode(const Bytecode &Code) {
	BytecodeValidationReport Report;
	if(Code.empty())
		Report.Warnings.push_back("Bytecode program is empty.");
	if(!Code.empty() && Code.back().Opcode_ != Opcode::HALT)
		Report.Warnings.push_back("Program does not end with HALT (last opcode is " +
		                          OpcodeName(Code.back().Opcode_) + ").");
	const auto Analysis = AnalyzeBytecode(Code);
	if(!Analysis.HasHalt)
		Report.Warnings.push_back("No HALT instruction found.");
	if(Analysis.HasDdl && Analysis.HasDml)
		Report.Warnings.push_back("Program mixes DDL and DML (may require careful transaction boundaries).");
	return Report;
}

} // namespace SQL
} // namespace AstralDB
