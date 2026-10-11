#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/structure/operation.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace markov::trace_graph::cost {

/** Binding evidence for phase operators, not total DAG or E2E coverage.
 * Summed service excludes residual gaps and is not critical-path time.
 */
struct PhaseCostCoverage {
    size_t migrated_operations = 0;
    double migrated_service_us = 0;
};

/** One resolved cost for an existing or generated operation slot.
 * This value carries no topology mutation or observed execution coordinate.
 */
struct PhaseOperatorCost {
    size_t node_id;
    uint64_t duration_us;
};

/** Migrate remaining unchanged phase work after semantic owners have removed
 * their generated/replaced regions. The structure layer has established a
 * unchanged work for every remaining source-node key before this call; changed
 * unowned nodes must not enter cost inference with proxy phase geometry.
 */
[[nodiscard]] std::vector<PhaseOperatorCost> bind_phase_cost_migrations(const core::DagGraph & source_graph,
                                                                        const std::map<size_t, structure::CostMigration> & operations,
                                                                        PhaseCostCoverage & coverage);

} // namespace markov::trace_graph::cost
