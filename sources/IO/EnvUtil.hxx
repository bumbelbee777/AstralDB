#pragma once

#include <cstdlib>

namespace AstralDB {

[[nodiscard]] inline const char *EnvGet(const char *Name) noexcept {
#if defined(_WIN32)
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
	const char *V = std::getenv(Name);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#else
	const char *V = std::getenv(Name);
#endif
	return V;
}

[[nodiscard]] inline bool EnvTruthy(const char *Name) noexcept {
	const char *V = EnvGet(Name);
	return V != nullptr && V[0] != '\0' && V[0] != '0';
}

} // namespace AstralDB
