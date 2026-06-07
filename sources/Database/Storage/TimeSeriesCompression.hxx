#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace TimeSeriesCompression {

constexpr size_t MaxSeriesLen = 1u << 20;

/** Delta + zigzag pack of numeric series → \c TSC[…] cell. */
std::string CompressValues(const std::vector<double> &Values);
std::optional<std::vector<double>> DecompressValues(std::string_view Cell);

/** Aligned epoch + value pairs (epoch seconds as doubles) → \c TSC[…] cell. */
std::string CompressSeries(const std::vector<double> &Epochs, const std::vector<double> &Values);
std::optional<std::pair<std::vector<double>, std::vector<double>>> DecompressSeries(std::string_view Cell);

} // namespace TimeSeriesCompression
} // namespace AstralDB
