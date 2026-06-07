#include <Database/Execution/FastPathGuard.hxx>

#include <IO/EnvUtil.hxx>

namespace AstralDB {

bool VerifyFastPathEnabled() noexcept { return EnvTruthy("ASTRALDB_VERIFY_FAST_PATH"); }

} // namespace AstralDB
