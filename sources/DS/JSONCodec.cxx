#include <DS/JSON.hxx>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

namespace AstralDB {
namespace DS {

std::optional<JSON> TryDecodeJSON(std::string_view S) noexcept {
	try {
		return DecodeJSONStrict(S);
	} catch(...) {
		return std::nullopt;
	}
}

} // namespace DS
} // namespace AstralDB

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
