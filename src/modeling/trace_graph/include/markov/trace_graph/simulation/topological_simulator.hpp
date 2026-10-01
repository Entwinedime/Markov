/**
 * @file
 * @brief Topological DAG simulation entry point and result.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

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
    size_t cpu_queue_count = 0;
    size_t cpu_task_count = 0;
    size_t max_cpu_queue_depth = 0;
    size_t submission_overlap_count = 0;
    uint64_t submission_overlap_total_us = 0;
    uint64_t submission_overlap_max_us = 0;
};

/** @brief Returns the edge delay used by simulation and critical-path reconstruction. */
[[nodiscard]] inline uint64_t topological_edge_delay_us(const core::DagNode & source, core::DagEdgeKind kind) noexcept {
    constexpr uint64_t kMaxPlausibleCpuGapUs = 1'000'000'000;
    if (kind != core::DagEdgeKind::Sequential || !source.is_cpu) return 0;
    return source.cpu_gap_after <= kMaxPlausibleCpuGapUs ? source.cpu_gap_after : 0;
}

/** Full-replay edge cost, including optional normal CPU service measurements. */
[[nodiscard]] inline uint64_t topological_edge_delay_us(const core::DagGraph & graph, const core::DagNode & source, core::DagEdgeKind kind) {
    return topological_edge_delay_us(source, kind) ? graph.cpu_service_gap_duration(source.id) : 0;
}

/** Compute the current node's duration at its actual start, after dependencies
 * and CPU queue admission. Calls follow nondecreasing simulated time (ties use
 * node id after ready dependencies). Return the complete duration, not a delta.
 * The result is stored on the node and immediately affects its successors.
 * The callback must not mutate graph topology or other node costs. Reusing a
 * stateful model requires a fresh state and the intended input graph per run.
 * A decision about future visibility is pending until its completion boundary;
 * apply shared-state changes at that boundary, not at the start of a wait.
 * This is a causal cost hook, not support for live structural DAG rewriting.
 */
using NodeCostAtStart = std::function<uint64_t(size_t node_id, uint64_t start_us, uint64_t duration_us)>;

/** Only valid during the expansion callback. New work depends on the current
 * node's completion and inherits its rank. Further edges may constrain nodes
 * that have not started; completed/running work is never changed. Native CPU
 * task identities remain fixed: append does not accept explicit CPU tasks and
 * depend does not create Correlation edges. append_cpu_task registers additional
 * work on an existing worker queue. Invalid edits abort this replay, without rollback.
 */
enum class BoundaryOrder { Ordinary, AfterConcurrentStarts };
class FutureDag {
public:
    virtual ~FutureDag() = default;
    // A zero-cost observation may run after ordinary starts at the same time.
    // This resolves simultaneous state changes without adding artificial time.
    // owner optionally supplies rank/PID for multi-rank work emitted together;
    // it does not add an execution dependency. The current boundary still does.
    virtual size_t append(const core::DagSyntheticNodeSpec & node, BoundaryOrder order = BoundaryOrder::Ordinary,
                          std::optional<size_t> owner = std::nullopt) = 0;
    /** Add one measured task to an existing recognized worker FIFO. The unique
     * cross-thread submission must not have completed. queue_member proves
     * the worker identity; no observed task or queue order is overwritten. */
    virtual size_t append_cpu_task(const core::DagSyntheticNodeSpec & node, size_t submission, size_t queue_member) = 0;
    virtual void depend(size_t predecessor, size_t successor, core::DagEdgeKind kind = core::DagEdgeKind::Mutation) = 0;
};
using ExpandAtStart = std::function<void(size_t node_id, uint64_t start_us, FutureDag & future)>;
/** Resolves correlated CPU task queues and replays the active DAG. With no hooks,
 * this executes existing work only. Proven inter-task CPU lane order is a resource
 * schedule, not a fixed causal dependency: full replay replaces it by arrival-order
 * FIFO and writes the chosen order back to the graph. Other edges remain hard
 * dependencies. Invalid endpoints, cycles and stalled queues throw without a result.
 *
 * Expansion runs before cost_at_start. All topology changes go through future;
 * callbacks must not mutate graph storage themselves. The resulting graph keeps
 * the generated work and can be replayed without expansion for verification.
 */
[[nodiscard]] SimulationResult run_topological_simulation(core::DagGraph & graph, const NodeCostAtStart & cost_at_start = {},
                                                          const ExpandAtStart & expand_at_start = {});

/**
 * @brief Replays the active DAG while contracting source snapshot and model blackbox spans.
 *
 * This pass never overwrites full-replay node timestamps. It is an independent
 * metric used only for snapshot/PREFILL/DECODE-stripped control deltas.
 */
[[nodiscard]] SimulationResult run_control_topological_simulation(core::DagGraph & graph);

/** @brief Replays Direct and phase work while removing the separately deferred CPU-gap component. */
[[nodiscard]] SimulationResult run_gap_excluded_topological_simulation(core::DagGraph & graph);

} // namespace markov::trace_graph::simulation
