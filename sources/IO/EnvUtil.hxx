#pragma once

#include <cstdlib>

namespace AstralDB {

[[nodiscard]] inline bool EnvTruthy(const char *Name) noexcept {
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

} // namespace AstralDB
