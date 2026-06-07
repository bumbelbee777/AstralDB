#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace AstralDB {

/** Allocation-free XML extract / validate (simdxml-style). */
struct SimdXmlExtract {
	static bool Extract(std::string_view Xml, std::string_view Path, std::string &Out);
	static bool Valid(std::string_view Xml) noexcept;
};

} // namespace AstralDB
