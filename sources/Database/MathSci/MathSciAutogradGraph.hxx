#pragma once

#include <Database/MathSci/MathSciAutograd.hxx>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciAutogradGraph {

using MathSciAutograd::ActSaveKind;
using MathSciAutograd::CompGraph;
using MathSciAutograd::GraphBackwardResult;
using MathSciAutograd::GraphForwardResult;
using MathSciAutograd::GraphPlan;

CompGraph FuseGraph(const CompGraph &Graph);
GraphPlan BuildGraphPlan(const CompGraph &Graph);
std::size_t GraphCachePayloadBytes(const GraphForwardResult &Forward);

std::optional<GraphForwardResult> ForwardGraphF32(const CompGraph &Graph, const std::vector<std::vector<float>> &Inputs);
std::optional<GraphBackwardResult> BackwardGraphF32(const CompGraph &Graph, const GraphForwardResult &Forward,
                                                    const std::vector<float> &Upstream);

std::optional<std::string> FormatGraphCache(const GraphForwardResult &Forward, std::uint16_t InputCount);
std::optional<GraphForwardResult> ParseGraphCache(std::string_view Cell);

} // namespace MathSciAutogradGraph
} // namespace AstralDB
