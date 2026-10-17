#pragma once

#include "markov/performance/cost_model.hpp"
#include "markov/performance/node_durations.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace markov {

struct CostQuery;

struct NodeCostEstimate {
    std::int64_t duration_ns;
    NodeCostBasis basis;
    std::optional<std::size_t> model_index = std::nullopt;
};

struct NodeCostSource {
    const std::filesystem::path * manifest;
    SourceSpanId span;
};

// One read-only query path for the execution kernel. The workflow owns given
// inputs and models for the entire combination; unknown work has no default.
class NodeCosts {
public:
    NodeCosts(const NodeDurations & given, std::span<const CostModel * const> models) : given_(given), models_(models) {}

    std::expected<NodeCostEstimate, std::string_view> estimate(const CostQuery & query, const NodeCostSource & source) const;
    std::expected<NodeCostEstimate, CostModelGap> query_cost(const CostQuery & query) const;
    bool has_models() const { return !models_.empty(); }
    static std::string_view basis_name(NodeCostBasis basis);

private:
    const NodeDurations & given_;
    std::span<const CostModel * const> models_;
};

} // namespace markov
