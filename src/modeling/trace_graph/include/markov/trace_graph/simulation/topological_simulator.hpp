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

/** @brief Returns the edge delay used by simulation and critical-path reconstruction. */
[[nodiscard]] inline uint64_t topological_edge_delay_us(const core::DagNode & source, core::DagEdgeKind kind) noexcept {
    constexpr uint64_t kMaxPlausibleCpuGapUs = 1'000'000'000;
    if (kind != core::DagEdgeKind::Sequential || !source.is_cpu) return 0;
    return source.cpu_gap_after <= kMaxPlausibleCpuGapUs ? source.cpu_gap_after : 0;
}

/**
 * @brief Replays an already constructed active DAG in topological order.
 *
 * Every active edge is a hard dependency: a destination cannot start before its
 * source completes. Invalid endpoints and cycles throw; no partial result is returned.
 */
[[nodiscard]] SimulationResult run_topological_simulation(core::DagGraph & graph);

/**
 * @brief Replays the active DAG while contracting source snapshot and model blackbox spans.
 *
 * This pass never overwrites full-replay node timestamps. It is an independent
 * metric used only for snapshot/PREFILL/DECODE-stripped control deltas.
 */
[[nodiscard]] SimulationResult run_control_topological_simulation(core::DagGraph & graph);

} // namespace markov::trace_graph::simulation
