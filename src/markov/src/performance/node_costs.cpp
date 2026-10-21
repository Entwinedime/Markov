#include "markov/performance/node_costs.hpp"

#include "markov/performance/cost_query.hpp"

#include <utility>
#include <variant>

namespace markov {

NodeCostResult NodeCosts::query_cost(const CostQuery & query) const {
    const CollectiveWork * collective = nullptr;
    if (const auto * work = std::get_if<CollectiveCostFeatures>(&query.features)) collective = &work->work;
    else if (const auto * work = std::get_if<CpuServiceWork>(&query.features); work && work->collective) collective = &*work->collective;

    // A logical message can drive conditional execution before its native
    // encoding has a qualified extent. It cannot match a measured cost yet.
    if (collective && collective->host_message && !collective->host_message->wire_bytes)
        return std::unexpected(CostModelGap{ CostQueryIssue::FeaturesUndefined, "host_message_wire_work_not_qualified" });

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

std::expected<NodeCostEstimate, std::string_view> NodeCosts::estimate(const NodeCostResult & measured, const CostQuery & query,
                                                                      const std::filesystem::path & manifest) const {
    if (measured) return *measured;
    if (query.issue != CostQueryIssue::None) return std::unexpected(query.contract_gap);

    const auto duration = given_.duration(manifest, query);
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
    case NodeCostBasis::CpuCandidate:
        return "empirical_cpu_service_candidate";
    }
    std::unreachable();
}

} // namespace markov
