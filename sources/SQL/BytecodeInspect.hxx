#pragma once

#include <SQL/Bytecode.hxx>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace AstralDB {
namespace SQL {

std::string OpcodeName(Opcode Op);

std::string DisassemblePretty(const Bytecode &Code);

enum class BytecodeAspectKind {
	Stats,
	Ddl,
	Dml,
	Tables,
	Sequences,
	Opcodes,
	SideEffects,
	Transactions,
	Joins,
	Aggregates,
	Security,
	All
};

struct BytecodeAnalysis {
	std::size_t InstructionCount = 0;
	std::size_t PureInstructionCount = 0;
	std::size_t SideEffectInstructionCount = 0;
	bool HasHalt = false;
	bool HasDdl = false;
	bool HasDml = false;
	bool HasTransactionControl = false;
	bool HasJoins = false;
	bool HasAggregates = false;
	bool HasWindowAnalytics = false;
	bool HasSecurityGrants = false;
	std::map<std::string, std::size_t> OpcodeHistogram;
	std::vector<std::string> ReferencedTables;
	std::vector<std::string> ReferencedSequences;
	std::vector<std::string> ReferencedRoles;
	std::vector<std::string> ScratchTables;
};

BytecodeAnalysis AnalyzeBytecode(const Bytecode &Code);

std::string FormatBytecodeAnalysis(const BytecodeAnalysis &Analysis);

/** Single-aspect query (case-insensitive \p AspectName). Throws on unknown aspect. */
std::string QueryBytecodeAspect(const Bytecode &Code, std::string_view AspectName);

BytecodeAspectKind ParseBytecodeAspectKind(std::string_view AspectName);

std::string FormatBytecodeAspect(const BytecodeAnalysis &Analysis, BytecodeAspectKind Kind);

struct BytecodeValidationReport {
	bool Ok = true;
	std::vector<std::string> Errors;
	std::vector<std::string> Warnings;
};

BytecodeValidationReport ValidateBytecode(const Bytecode &Code);

} // namespace SQL
} // namespace AstralDB
