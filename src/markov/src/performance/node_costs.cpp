#include "markov/performance/node_costs.hpp"

#include "markov/performance/cost_query.hpp"

#include <utility>

namespace markov {

std::expected<NodeCostEstimate, CostModelGap> NodeCosts::query_cost(const CostQuery & query) const {
    CostModelGap gap;
    for (std::size_t index = 0; index < models_.size(); ++index) {
        const auto * model = models_[index];
        const auto estimate = model->estimate(query);
        if (estimate) return NodeCostEstimate{ *estimate, model->basis(), index };
        if (gap.reason.empty() && !estimate.error().reason.empty()) gap = estimate.error();
    }
    // Later candidates can still provide a valid value after an earlier
    // contract gap. Preserve the first actual lookup reason only if all fail.
    return std::unexpected(gap);
}

std::expected<NodeCostEstimate, std::string_view> NodeCosts::estimate(const CostQuery & query, const NodeCostSource & source) const {
    if (auto cost = query_cost(query)) return *cost;

    const auto duration = given_.duration(*source.manifest, source.span);
    if (!duration) return std::unexpected(has_models() ? "node_cost_missing" : "given_node_duration_missing");
    return NodeCostEstimate{ *duration, NodeCostBasis::Given };
}

std::string_view NodeCosts::basis_name(NodeCostBasis basis) {
    switch (basis) {
    case NodeCostBasis::Given:
        return "given_node_durations";
    case NodeCostBasis::KernelCandidate:
        return "empirical_kernel_candidate";
    case NodeCostBasis::CommunicationCandidate:
        return "empirical_communication_candidate";
    case NodeCostBasis::TransferCandidate:
        return "empirical_transfer_candidate";
    case NodeCostBasis::DeviceCommandCandidate:
        return "empirical_device_command_candidate";
    }
    std::unreachable();
}

} // namespace markov
