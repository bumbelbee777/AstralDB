#pragma once

#include <SQL/Bytecode/Bytecode.hxx>
#include <IO/Logger.hxx>

namespace AstralDB {
namespace SQL {

/** Lower scan/join/aggregate bytecode on INSERT BULK tables to dedicated *_BULK opcodes. */
class BulkOpcodePass : public OptimizationPass {
public:
	bool Run(Bytecode &Code, Logger *Logger = nullptr) override;
	const char *GetName() const override { return "BulkOpcode"; }
};

} // namespace SQL
} // namespace AstralDB
