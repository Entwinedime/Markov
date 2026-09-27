#pragma once
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"

namespace markov::trace_graph::modules::hicache::patch {

struct HiCacheLayerWaitInsertion {
    const HiCacheLayerWaitCall * call = nullptr;
    core::DagNodeRef layer_ready;
};

struct HiCacheLayerWaitPosition {
    std::vector<size_t> before, synchronizations;
    size_t after = 0, device_lane = 0;
    std::string issue;
};

/** Locate inactive calls on the current graph, including after active-wait
 * removal. Build submission indices once rather than scanning per call. */
std::map<const HiCacheLayerWaitCall *, HiCacheLayerWaitPosition> observe_inactive_layer_positions(
    const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
    std::span<const HiCacheLayerWaitCall * const> calls);

struct HiCacheLayerWaitInsertionPlan {
    std::string status = "unavailable";
    std::map<std::string, size_t> issues;
    size_t inserted_calls = 0;
    std::map<const HiCacheLayerWaitCall *, core::DagNodeRef> waits;
    core::DagMutationPlan mutation{ .component = "hicache_layer_wait" };
};

/** Place inactive-to-active calls using source program order and measured call costs.
 * layer_ready must be the caller's predicted per-layer readiness, never a target trace.
 * This returns a separate transaction; the caller combines it atomically with I/O.
 */
HiCacheLayerWaitInsertionPlan plan_hicache_layer_wait_insertions(const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
                                                                 const std::vector<HiCacheLayerWaitInsertion> & insertions);

} // namespace markov::trace_graph::modules::hicache::patch
