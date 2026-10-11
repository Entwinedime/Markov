#include "markov/trace_graph/cost/hicache_phase.hpp"

namespace markov::trace_graph::cost {

std::vector<PhaseOperatorCost> bind_phase_cost_migrations(const core::DagGraph & source_graph, const std::map<size_t, structure::CostMigration> & operations,
                                                          PhaseCostCoverage & coverage) {
    std::vector<PhaseOperatorCost> costs;
    costs.reserve(operations.size());
    for (const auto & [node, binding] : operations) {
        const auto duration = source_graph.cpu_service_node_duration(node);
        ++coverage.migrated_operations;
        coverage.migrated_service_us += duration;

        costs.push_back({ .node_id = node, .duration_us = duration });
    }
    return costs;
}

} // namespace markov::trace_graph::cost
