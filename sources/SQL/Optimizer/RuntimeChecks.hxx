#pragma once

#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Optimizer/ConfidenceScoring.hxx>

namespace AstralDB {
namespace SQL {

/** Inserts lightweight runtime verification hooks for speculative optimizations. */
class RuntimeCheckGenerator {
public:
	void InsertChecks(Bytecode &Code, Confidence Level);
	void InsertPreFilterCheck(Bytecode &Code, std::size_t Ip);
	void InsertPostAggCheck(Bytecode &Code, std::size_t Ip);

private:
	static Instruction MakeCheckInstruction(Confidence Level, int64_t Tag);
};

} // namespace SQL
} // namespace AstralDB
