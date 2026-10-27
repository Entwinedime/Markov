#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/structure/operation.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace markov::trace_graph::cost {

/** Binding evidence for source operators, not total DAG or E2E coverage.
 * Summed service excludes residual gaps and is not critical-path time.
 */
struct SourceCostCoverage {
    size_t migrated_operations = 0;
    double migrated_service_us = 0;
};

/** One resolved cost for an existing or generated operation slot.
 * This value carries no topology mutation or observed execution coordinate.
 */
struct SourceNodeCost {
    size_t node_id;
    uint64_t duration_us;
};

/** Migrate remaining unchanged work after semantic owners have removed
 * their generated/replaced regions. The structure layer has established unchanged work for every remaining source-node key before this call; changed
 * unowned nodes must not enter cost inference with proxy phase geometry.
 */
[[nodiscard]] std::vector<SourceNodeCost> migrate_source_costs(const core::DagGraph & source_graph,
                                                               const std::map<size_t, structure::CostMigration> & operations, SourceCostCoverage & coverage);

} // namespace markov::trace_graph::cost
