/**
 * @file
 * @brief Constructs the faithful base execution DAG from Chrome trace events.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <vector>

namespace markov::trace_graph::core {

/**
 * @brief Converts normalized trace events into a simulatable base DAG.
 *
 * `DagBuilder` owns only dependencies justified by faithful replay:
 * - sequential ordering on the merged CPU lane;
 * - stream ordering on each device lane;
 * - runtime-launch to kernel correlation;
 * - event, stream, and device synchronization;
 * - request-lifecycle serialization proven by workload-identity facts;
 * - model-execution synchronization anchors such as notify events.
 *
 * What-if policy belongs in a `SimulationModule` after construction, not in this builder.
 */
class DagBuilder {
public:
    /** @brief Sets maximum phase parallelism within one logical input. */
    explicit DagBuilder(size_t threads = 1);

    /** @brief Builds the base DAG for one rank and one logical trace input. */
    [[nodiscard]] DagGraph build(std::vector<TraceEvent> events, int gpu_id) const;

private:
    size_t threads_ = 1;
};

} // namespace markov::trace_graph::core
