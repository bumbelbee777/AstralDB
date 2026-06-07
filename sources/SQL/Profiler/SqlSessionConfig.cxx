#include <SQL/Profiler/SqlSessionConfig.hxx>

#include <SQL/Bytecode/FastPathGuard.hxx>

#include <cstdlib>
#include <cstring>

namespace AstralDB {
namespace SQL {

namespace {

bool EnvTruthy(const char *Name) noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	const char *V = std::getenv(Name);
#pragma warning(pop)
#else
	const char *V = std::getenv(Name);
#endif
	return V != nullptr && V[0] != '\0' && V[0] != '0';
}

int EnvInt(const char *Name, int Default) noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	const char *V = std::getenv(Name);
#pragma warning(pop)
#else
	const char *V = std::getenv(Name);
#endif
	if(!V || !*V)
		return Default;
	return std::atoi(V);
}

OptimizationLevel OptLevelFromEnv() {
	const int L = EnvInt("ASTRALDB_OPT_LEVEL", 2);
	if(L <= 0)
		return OptimizationLevel::None;
	if(L == 1)
		return OptimizationLevel::Basic;
	if(L == 2)
		return OptimizationLevel::Advanced;
	if(L == 3)
		return OptimizationLevel::Aggressive;
	return OptimizationLevel::Maximum;
}

SqlSessionConfig::VmChoice VmFromEnv() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	const char *V = std::getenv("ASTRALDB_VM");
#pragma warning(pop)
#else
	const char *V = std::getenv("ASTRALDB_VM");
#endif
	if(!V)
		return SqlSessionConfig::VmChoice::Auto;
	if(std::strcmp(V, "DBVM") == 0 || std::strcmp(V, "dbvm") == 0)
		return SqlSessionConfig::VmChoice::Dbvm;
	if(std::strcmp(V, "SILLY") == 0 || std::strcmp(V, "silly") == 0)
		return SqlSessionConfig::VmChoice::Silly;
	if(std::strcmp(V, "JIT") == 0 || std::strcmp(V, "jit") == 0)
		return SqlSessionConfig::VmChoice::Jit;
	return SqlSessionConfig::VmChoice::Auto;
}

} // namespace

SqlSessionConfig SqlSessionConfig::FromEnvironment() {
	SqlSessionConfig Cfg;
	Cfg.OptLevel = OptLevelFromEnv();
	Cfg.Vm = VmFromEnv();
	Cfg.VectorBatchSize = static_cast<std::uint32_t>(EnvInt("ASTRALDB_VEC_BATCH_SIZE", 1024));
	Cfg.VerifyFastPath = VerifyFastPathEnabled();
	Cfg.ZoneMapAdaptive = EnvTruthy("ASTRALDB_ZONE_MAP_ADAPTIVE");
	if(!EnvTruthy("ASTRALDB_ZONE_MAP_ADAPTIVE") && std::getenv("ASTRALDB_ZONE_MAP_ADAPTIVE") != nullptr)
		Cfg.ZoneMapAdaptive = false;
	Cfg.UsePrecomputed = true;
	if(std::getenv("ASTRALDB_USE_PRECOMPUTED") != nullptr)
		Cfg.UsePrecomputed = EnvTruthy("ASTRALDB_USE_PRECOMPUTED");
	if(const char *P = std::getenv("ASTRALDB_PROFILE_OUTPUT")) {
		if(P[0])
			Cfg.ProfileDumpPath = P;
	}
	if(const char *N = std::getenv("ASTRALDB_PROFILE_NAME")) {
		if(N[0])
			Cfg.ProfileName = N;
	}
	Cfg.JitEnabled = EnvInt("ASTRALDB_JIT", 1) != 0;
	return Cfg;
}

} // namespace SQL
} // namespace AstralDB
