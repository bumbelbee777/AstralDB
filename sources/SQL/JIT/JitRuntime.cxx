#include <SQL/JIT/JitRuntime.hxx>

#include <SQL/JIT/JitCompiler.hxx>
#include <SQL/JIT/JitInvoke.hxx>
#include <Database/Storage/VectorizedOps.hxx>

namespace AstralDB {
namespace SQL {

void FilterI64Batch(FilterCompareOp Op, int64_t Literal, const int64_t *Values, std::size_t Count,
                    std::vector<std::size_t> &OutIndices) {
	OutIndices.clear();
	if(!Values || Count == 0)
		return;

	if(JitCompiler::Enabled()) {
		const auto Fn = JitCompiler::Instance().CompileFilterDense(Op, Literal);
		if(Fn) {
			OutIndices.resize(Count);
			const std::size_t Hit = JitInvokeFilter(Fn, Values, Count, Literal, OutIndices.data());
			OutIndices.resize(Hit);
			return;
		}
	}

	VectorizedFilter Filter(Values, Count, Op, Literal);
	Filter.ExecuteIndices(OutIndices);
}

} // namespace SQL
} // namespace AstralDB
