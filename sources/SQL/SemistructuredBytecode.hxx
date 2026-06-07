#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {
namespace SQL {

enum class SSOpcode : std::uint8_t {
	JSON_EXTRACT_BATCH,
	XML_EXTRACT_BATCH,
	REGEX_BATCH,
	FTS_MATCH_BATCH,
	RANK_BATCH,
	TOPK_INIT,
	TOPK_INSERT_BATCH,
	TOPK_MATERIALIZE,
	MATERIALIZE_BATCH,
	FUSED_EXECUTE,
};

struct SSInstruction {
	SSOpcode Op = SSOpcode::FUSED_EXECUTE;
	std::vector<int32_t> IntOperands;
	std::vector<std::string> StrOperands;
	std::vector<std::uint8_t> BlobOperands;
};

struct SSProgram {
	std::vector<SSInstruction> Instructions;
	std::uint32_t BatchSize = 1024;
	bool UseSimd = true;
	bool UseParallel = true;
};

} // namespace SQL
} // namespace AstralDB
