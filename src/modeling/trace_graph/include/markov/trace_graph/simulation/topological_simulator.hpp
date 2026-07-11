/**
 * @file
 * @brief Topological DAG simulation entry point and result.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <cstdint>

namespace markov::trace_graph::simulation {

/**
 * @brief Successful topological simulation result.
 *
 * `e2e_us` is the simulated DAG critical-path length in Chrome trace microseconds;
 * it is not the observed input timestamp window.
 */
struct SimulationResult {
    uint64_t e2e_us = 0;
    size_t processed_nodes = 0;
};

/**
 * @brief Replays an already constructed active DAG in topological order.
 *
 * Every active edge is a hard dependency: a destination cannot start before its
 * source completes. Invalid endpoints and cycles throw; no partial result is returned.
 */
[[nodiscard]] SimulationResult run_topological_simulation(core::DagGraph & graph);

} // namespace markov::trace_graph::simulation
