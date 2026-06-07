#pragma once

#include <SQL/Bytecode/Bytecode.hxx>

namespace AstralDB {
namespace SQL {

/** Confidence tier for speculative optimizations and runtime checks. */
enum class Confidence : uint8_t {
	Reject = 0,
	Low = 1,
	Medium = 2,
	High = 3,
	Certain = 4
};

class ConfidenceScorer {
public:
	Confidence ScoreBytecode(const Bytecode &Code) const;
	Confidence ScoreFusionPlan(bool HasColumnarHint, std::size_t FilterCount, bool HasJoin) const;

private:
	static bool IsSideEffectFreeScan(const Instruction &Inst);
};

} // namespace SQL
} // namespace AstralDB
