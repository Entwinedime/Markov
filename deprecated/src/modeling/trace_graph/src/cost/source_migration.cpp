#include "markov/trace_graph/cost/source_migration.hpp"

namespace markov::trace_graph::cost {

std::vector<SourceNodeCost> migrate_source_costs(const core::DagGraph & source_graph, const std::map<size_t, structure::CostMigration> & operations,
                                                 SourceCostCoverage & coverage) {
    std::vector<SourceNodeCost> costs;
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
