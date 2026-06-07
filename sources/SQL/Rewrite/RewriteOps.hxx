#pragma once

#include <SQL/Bytecode/BytecodeInterpreter.hxx>

namespace AstralDB {
namespace SQL {

/** VM handlers for rewrite-specific opcodes. */
class RewriteOps {
public:
	static bool HandleRecursiveCteBfs(BytecodeInterpreter &Vm, const Bytecode &Code, std::size_t FixIp,
	                                  const Instruction &Inst);
	static bool HandleSemiJoinHash(BytecodeInterpreter &Vm, const Instruction &Inst);
	static bool HandleHierarchyPathScan(BytecodeInterpreter &Vm, const Instruction &Inst);

	static bool SemiJoinProbe(Database &Db, const std::string &OuterTable, const std::string &InnerTable,
	                          const std::string &OuterKey, const std::string &InnerKey, bool Negated,
	                          const std::string &InnerFilterDnf, const std::unordered_map<std::string, std::string> &OuterRow);
};

} // namespace SQL
} // namespace AstralDB
