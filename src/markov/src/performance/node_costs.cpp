#include "markov/performance/node_costs.hpp"

#include <utility>

namespace markov {

std::expected<NodeCostEstimate, std::string_view> NodeCosts::estimate(const ServiceWork & work) const {
    for (std::size_t index = 0; index < models_.size(); ++index) {
        const auto * model = models_[index];
        if (const auto duration = model->estimate(work)) return NodeCostEstimate{ *duration, model->basis(), index };
    }

    // Remaining explicit values are conditional structural inputs. Their use
    // is reported separately and cannot imply measured cost coverage.
    if (const auto duration = given_.duration(work)) return NodeCostEstimate{ *duration, NodeCostBasis::Given };
    return std::unexpected(has_models() ? "node_cost_missing" : "given_node_duration_missing");
}

std::string_view NodeCosts::basis_name(NodeCostBasis basis) {
    switch (basis) {
    case NodeCostBasis::Given:
        return "given_node_durations";
    case NodeCostBasis::AdmissionCandidate:
        return "empirical_admission_candidate";
    case NodeCostBasis::WorkerCandidate:
        return "empirical_worker_candidate";
    case NodeCostBasis::KernelCandidate:
        return "empirical_kernel_candidate";
    case NodeCostBasis::CommunicationCandidate:
        return "empirical_communication_candidate";
    }
    std::unreachable();
}

} // namespace markov
