#include <SQL/BytecodeInspect.hxx>
#include <algorithm>
#include <cctype>
#include <iomanip>
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

std::string OperandPreview(const Value &V) {
	return std::visit(
	    [](const auto &Arg) -> std::string {
		    using T = std::decay_t<decltype(Arg)>;
		    if constexpr(std::is_same_v<T, int64_t>)
			    return std::to_string(Arg);
		    else if constexpr(std::is_same_v<T, double>)
			    return std::to_string(Arg);
		    else
			    return "\"" + std::string(Arg) + "\"";
	    },
	    V);
}

bool OpcodeIsDdl(Opcode Op) {
	switch(Op) {
	case Opcode::CREATE_TABLE:
	case Opcode::DROP_TABLE:
	case Opcode::CREATE_VIEW:
	case Opcode::DROP_VIEW:
	case Opcode::ALTER_TABLE:
	case Opcode::RENAME_TABLE:
	case Opcode::CREATE_INDEX:
	case Opcode::DROP_INDEX:
	case Opcode::CREATE_SCHEMA:
	case Opcode::DROP_SCHEMA:
	case Opcode::ALTER_SCHEMA:
	case Opcode::CREATE_SEQUENCE:
	case Opcode::DROP_SEQUENCE:
	case Opcode::CREATE_ROLE:
	case Opcode::DROP_ROLE:
	case Opcode::CREATE_PROCEDURE:
	case Opcode::DROP_PROCEDURE:
		return true;
	default:
		return false;
	}
}

bool OpcodeIsDml(Opcode Op) {
	switch(Op) {
	case Opcode::INSERT:
	case Opcode::INSERT_BULK:
	case Opcode::UPDATE:
	case Opcode::DELETE:
	case Opcode::UPDATE_MATCHING:
	case Opcode::DELETE_MATCHING:
	case Opcode::UPSERT:
	case Opcode::MERGE_INTO:
		return true;
	default:
		return false;
	}
}

bool OpcodeIsJoin(Opcode Op) {
	switch(Op) {
	case Opcode::INNER_JOIN:
	case Opcode::LEFT_JOIN:
	case Opcode::RIGHT_JOIN:
	case Opcode::FULL_JOIN:
	case Opcode::CROSS_JOIN:
		return true;
	default:
		return false;
	}
}

bool OpcodeIsAggregate(Opcode Op) {
	switch(Op) {
	case Opcode::GROUP_BY:
	case Opcode::ROLLUP:
	case Opcode::CUBE:
	case Opcode::GROUPING_SETS:
		return true;
	default:
		return false;
	}
}

bool OpcodeIsSecurity(Opcode Op) {
	switch(Op) {
	case Opcode::GRANT:
	case Opcode::REVOKE:
	case Opcode::GRANT_ROLE_MEMBERSHIP:
	case Opcode::REVOKE_ROLE_MEMBERSHIP:
	case Opcode::GRANT_COLUMN:
	case Opcode::REVOKE_COLUMN:
		return true;
	default:
		return false;
	}
}

void HarvestOperands(const Instruction &Inst, BytecodeAnalysis &Analysis) {
	const auto FirstString = [&Inst]() -> const std::string * {
		for(const auto &Op : Inst.Operands)
			if(const auto *S = std::get_if<std::string>(&Op))
				return S;
		return nullptr;
	};
	switch(Inst.Opcode) {
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
	default:
		break;
	}
}

} // namespace

std::string OpcodeName(Opcode Op) {
	switch(Op) {
	case Opcode::SELECT:
		return "SELECT";
	case Opcode::INSERT:
		return "INSERT";
	case Opcode::INSERT_BULK:
		return "INSERT_BULK";
	case Opcode::UPDATE:
		return "UPDATE";
	case Opcode::DELETE:
		return "DELETE";
	case Opcode::CREATE_TABLE:
		return "CREATE_TABLE";
	case Opcode::DROP_TABLE:
		return "DROP_TABLE";
	case Opcode::SET:
		return "SET";
	case Opcode::WHERE:
		return "WHERE";
	case Opcode::ORDER_BY:
		return "ORDER_BY";
	case Opcode::GROUP_BY:
		return "GROUP_BY";
	case Opcode::LIMIT:
		return "LIMIT";
	case Opcode::OFFSET:
		return "OFFSET";
	case Opcode::KEEP_ROWS:
		return "KEEP_ROWS";
	case Opcode::DEDUP_ROWS:
		return "DEDUP_ROWS";
	case Opcode::SET_COMBINE:
		return "SET_COMBINE";
	case Opcode::DELETE_MATCHING:
		return "DELETE_MATCHING";
	case Opcode::UPDATE_MATCHING:
		return "UPDATE_MATCHING";
	case Opcode::UPSERT:
		return "UPSERT";
	case Opcode::MERGE_INTO:
		return "MERGE_INTO";
	case Opcode::FILTER_DNF:
		return "FILTER_DNF";
	case Opcode::PUSH_POOL:
		return "PUSH_POOL";
	case Opcode::AND:
		return "AND";
	case Opcode::OR:
		return "OR";
	case Opcode::NOT:
		return "NOT";
	case Opcode::EQ:
		return "EQ";
	case Opcode::NE:
		return "NE";
	case Opcode::LT:
		return "LT";
	case Opcode::LE:
		return "LE";
	case Opcode::GT:
		return "GT";
	case Opcode::GE:
		return "GE";
	case Opcode::ADD:
		return "ADD";
	case Opcode::SUB:
		return "SUB";
	case Opcode::MUL:
		return "MUL";
	case Opcode::DIV:
		return "DIV";
	case Opcode::MOD:
		return "MOD";
	case Opcode::PUSH:
		return "PUSH";
	case Opcode::POP:
		return "POP";
	case Opcode::LOAD:
		return "LOAD";
	case Opcode::STORE:
		return "STORE";
	case Opcode::CALL:
		return "CALL";
	case Opcode::RET:
		return "RET";
	case Opcode::JMP:
		return "JMP";
	case Opcode::NOP:
		return "NOP";
	case Opcode::HALT:
		return "HALT";
	case Opcode::GRANT:
		return "GRANT";
	case Opcode::REVOKE:
		return "REVOKE";
	case Opcode::CREATE_ROLE:
		return "CREATE_ROLE";
	case Opcode::DROP_ROLE:
		return "DROP_ROLE";
	case Opcode::CREATE_SEQUENCE:
		return "CREATE_SEQUENCE";
	case Opcode::DROP_SEQUENCE:
		return "DROP_SEQUENCE";
	case Opcode::GRANT_ROLE_MEMBERSHIP:
		return "GRANT_ROLE_MEMBERSHIP";
	case Opcode::REVOKE_ROLE_MEMBERSHIP:
		return "REVOKE_ROLE_MEMBERSHIP";
	case Opcode::GRANT_COLUMN:
		return "GRANT_COLUMN";
	case Opcode::REVOKE_COLUMN:
		return "REVOKE_COLUMN";
	case Opcode::BEGIN:
		return "BEGIN";
	case Opcode::COMMIT:
		return "COMMIT";
	case Opcode::ROLLBACK:
		return "ROLLBACK";
	case Opcode::INNER_JOIN:
		return "INNER_JOIN";
	case Opcode::LEFT_JOIN:
		return "LEFT_JOIN";
	case Opcode::RIGHT_JOIN:
		return "RIGHT_JOIN";
	case Opcode::FULL_JOIN:
		return "FULL_JOIN";
	case Opcode::CROSS_JOIN:
		return "CROSS_JOIN";
	case Opcode::WITH:
		return "WITH";
	case Opcode::WINDOW:
		return "WINDOW";
	case Opcode::PARTITION_BY:
		return "PARTITION_BY";
	case Opcode::OVER:
		return "OVER";
	case Opcode::CONCAT:
		return "CONCAT";
	case Opcode::SUBSTRING:
		return "SUBSTRING";
	case Opcode::TRIM:
		return "TRIM";
	case Opcode::LTRIM:
		return "LTRIM";
	case Opcode::RTRIM:
		return "RTRIM";
	case Opcode::UPPER:
		return "UPPER";
	case Opcode::LOWER:
		return "LOWER";
	case Opcode::REPLACE:
		return "REPLACE";
	case Opcode::REGEXP_MATCH:
		return "REGEXP_MATCH";
	case Opcode::DATE_ADD:
		return "DATE_ADD";
	case Opcode::DATE_SUB:
		return "DATE_SUB";
	case Opcode::DATE_DIFF:
		return "DATE_DIFF";
	case Opcode::EXTRACT_DATE:
		return "EXTRACT_DATE";
	case Opcode::EXTRACT_TIME:
		return "EXTRACT_TIME";
	case Opcode::JSON_EXTRACT:
		return "JSON_EXTRACT";
	case Opcode::JSON_CONTAINS:
		return "JSON_CONTAINS";
	case Opcode::JSON_MERGE:
		return "JSON_MERGE";
	case Opcode::MATCH:
		return "MATCH";
	case Opcode::AGAINST:
		return "AGAINST";
	case Opcode::ROLLUP:
		return "ROLLUP";
	case Opcode::CUBE:
		return "CUBE";
	case Opcode::GROUPING_SETS:
		return "GROUPING_SETS";
	case Opcode::EXISTS:
		return "EXISTS";
	case Opcode::IN:
		return "IN";
	case Opcode::ANY:
		return "ANY";
	case Opcode::ALL:
		return "ALL";
	case Opcode::CHECK_CONSTRAINT:
		return "CHECK_CONSTRAINT";
	case Opcode::FOREIGN_KEY:
		return "FOREIGN_KEY";
	case Opcode::CREATE_INDEX:
		return "CREATE_INDEX";
	case Opcode::DROP_INDEX:
		return "DROP_INDEX";
	case Opcode::CREATE_VIEW:
		return "CREATE_VIEW";
	case Opcode::DROP_VIEW:
		return "DROP_VIEW";
	case Opcode::CREATE_PROCEDURE:
		return "CREATE_PROCEDURE";
	case Opcode::DROP_PROCEDURE:
		return "DROP_PROCEDURE";
	case Opcode::CALL_PROCEDURE:
		return "CALL_PROCEDURE";
	case Opcode::CREATE_SCHEMA:
		return "CREATE_SCHEMA";
	case Opcode::DROP_SCHEMA:
		return "DROP_SCHEMA";
	case Opcode::ALTER_SCHEMA:
		return "ALTER_SCHEMA";
	case Opcode::ALTER_TABLE:
		return "ALTER_TABLE";
	case Opcode::RENAME_TABLE:
		return "RENAME_TABLE";
	case Opcode::SAVEPOINT:
		return "SAVEPOINT";
	case Opcode::ROLLBACK_TO:
		return "ROLLBACK_TO";
	case Opcode::RELEASE_SAVEPOINT:
		return "RELEASE_SAVEPOINT";
	case Opcode::EXPORT_DATABASE:
		return "EXPORT_DATABASE";
	case Opcode::IMPORT_DATABASE:
		return "IMPORT_DATABASE";
	case Opcode::CONVERT_TABULAR_FILES:
		return "CONVERT_TABULAR_FILES";
	case Opcode::CLONE_TABLE:
		return "CLONE_TABLE";
	case Opcode::FILTER_AS_OF:
		return "FILTER_AS_OF";
	case Opcode::MATCH_RECOGNIZE:
		return "MATCH_RECOGNIZE";
	case Opcode::RECURSIVE_CTE_FIXPOINT:
		return "RECURSIVE_CTE_FIXPOINT";
	case Opcode::WINDOW_ROW_NUMBER:
		return "WINDOW_ROW_NUMBER";
	case Opcode::SLICE_RANGE:
		return "SLICE_RANGE";
	case Opcode::STORAGE_HINT:
		return "STORAGE_HINT";
	case Opcode::CASE_EVAL:
		return "CASE_EVAL";
	case Opcode::CAST_EVAL:
		return "CAST_EVAL";
	case Opcode::SCALAR_FUNC_EVAL:
		return "SCALAR_FUNC_EVAL";
	}
	return "OP_" + std::to_string(static_cast<int>(Op));
}

std::string DisassemblePretty(const Bytecode &Code) {
	std::ostringstream Out;
	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Instruction &Inst = Code[I];
		Out << std::setw(5) << I << "  " << OpcodeName(Inst.Opcode);
		if(!Inst.Operands.empty()) {
			Out << "  ";
			for(std::size_t O = 0; O < Inst.Operands.size(); ++O) {
				if(O)
					Out << ", ";
				Out << OperandPreview(Inst.Operands[O]);
			}
		}
		Out << "\n";
	}
	return Out.str();
}

BytecodeAnalysis AnalyzeBytecode(const Bytecode &Code) {
	BytecodeAnalysis Analysis;
	Analysis.InstructionCount = Code.size();
	for(const Instruction &Inst : Code) {
		++Analysis.OpcodeHistogram[OpcodeName(Inst.Opcode)];
		if(Inst.IsPure())
			++Analysis.PureInstructionCount;
		if(Inst.HasSideEffects())
			++Analysis.SideEffectInstructionCount;
		if(Inst.Opcode == Opcode::HALT)
			Analysis.HasHalt = true;
		if(OpcodeIsDdl(Inst.Opcode))
			Analysis.HasDdl = true;
		if(OpcodeIsDml(Inst.Opcode))
			Analysis.HasDml = true;
		if(OpcodeIsJoin(Inst.Opcode))
			Analysis.HasJoins = true;
		if(OpcodeIsAggregate(Inst.Opcode))
			Analysis.HasAggregates = true;
		if(Inst.Opcode == Opcode::WINDOW_ROW_NUMBER)
			Analysis.HasWindowAnalytics = true;
		if(OpcodeIsSecurity(Inst.Opcode))
			Analysis.HasSecurityGrants = true;
		switch(Inst.Opcode) {
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
	if(!Code.empty() && Code.back().Opcode != Opcode::HALT)
		Report.Warnings.push_back("Program does not end with HALT (last opcode is " +
		                          OpcodeName(Code.back().Opcode) + ").");
	const auto Analysis = AnalyzeBytecode(Code);
	if(!Analysis.HasHalt)
		Report.Warnings.push_back("No HALT instruction found.");
	for(const Instruction &Inst : Code) {
		if(static_cast<int>(Inst.Opcode) > static_cast<int>(Opcode::SCALAR_FUNC_EVAL))
			Report.Errors.push_back("Unknown opcode value " + std::to_string(static_cast<int>(Inst.Opcode)));
	}
	Report.Ok = Report.Errors.empty();
	return Report;
}

} // namespace SQL
} // namespace AstralDB
