#include <SQL/Bytecode/FastPathGuard.hxx>
#include <Database/Execution/FastPathGuard.hxx>

#include <IO/EnvUtil.hxx>

#include <algorithm>

namespace AstralDB {

bool VerifyFastPathEnabled() noexcept { return EnvTruthy("ASTRALDB_VERIFY_FAST_PATH"); }

namespace SQL {

void RecordFastPathHit(BytecodeInterpreter::TimeSqlStats &Stats, FastPathFlag Flag, std::uint64_t MinScanned,
                       std::uint64_t MinResult) {
	Stats.FastPathFlags |= static_cast<std::uint32_t>(Flag);
	if(MinScanned > Stats.MinRowsScannedExpected)
		Stats.MinRowsScannedExpected = MinScanned;
	if(MinResult > Stats.MinResultRowsExpected)
		Stats.MinResultRowsExpected = MinResult;
}

void ValidateTimeSqlIntegrity(BytecodeInterpreter::TimeSqlStats &Stats, double ExecuteMs) {
	if(Stats.IntegrityFailed)
		return;

	const bool UsedFastPath = Stats.FastPathFlags != 0;
	const bool Strict = EnvTruthy("ASTRALDB_STRICT_FAST_PATH");

	if(UsedFastPath && Stats.MinRowsScannedExpected > 0 && Stats.RowsScanned < Stats.MinRowsScannedExpected) {
		Stats.IntegrityFailed = true;
		Stats.IntegrityMessage = "fast path scanned fewer rows than expected (possible silent skip)";
		return;
	}

	if(Stats.MinResultRowsExpected > 0 && Stats.ResultRows < Stats.MinResultRowsExpected) {
		Stats.IntegrityFailed = true;
		Stats.IntegrityMessage = "result row count below minimum expected";
		return;
	}

	if(UsedFastPath && ExecuteMs >= 0.0 && ExecuteMs < 0.005 && Stats.RowsScanned == 0 &&
	   Stats.MinRowsScannedExpected > 0) {
		Stats.IntegrityFailed = true;
		Stats.IntegrityMessage = "suspiciously fast execution with zero scanned_rows";
		return;
	}

	if(Strict && UsedFastPath && Stats.ResultRows == 0 && Stats.MinResultRowsExpected == 0 &&
	   Stats.MinRowsScannedExpected > 0) {
		Stats.IntegrityFailed = true;
		Stats.IntegrityMessage = "strict mode: fast path produced zero result rows";
	}
}

} // namespace SQL
} // namespace AstralDB
