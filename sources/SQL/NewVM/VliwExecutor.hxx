#pragma once

#include <SQL/NewVM/VliwBundle.hxx>

namespace AstralDB {
namespace SQL {

class VliwExecutor {
public:
	struct ExecContext {
		std::vector<int64_t> RegFile;
	};

	explicit VliwExecutor(ExecContext *Ctx = nullptr);

	void ExecuteBundle(const VliwBundle &Bundle);
	void ExecuteAll(const std::vector<VliwBundle> &Bundles, bool Parallel = true);

private:
	ExecContext *Ctx_;
	void ExecuteSlot(const VliwSlot &Slot);
};

} // namespace SQL
} // namespace AstralDB
