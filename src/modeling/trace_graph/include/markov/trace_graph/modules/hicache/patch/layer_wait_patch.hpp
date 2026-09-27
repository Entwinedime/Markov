#pragma once
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

namespace markov::trace_graph::modules::hicache::patch {

struct HiCacheLayerWaitPatch {
    std::string status = "unavailable";
    size_t retained_calls = 0, removed_calls = 0, required_insertions = 0;
    size_t inserted_calls = 0, rebound_calls = 0, expanded_loadbacks = 0;
    uint64_t removed_main_cpu_us = 0, removed_worker_us = 0, added_return_us = 0;
    std::map<std::string, size_t> blockers;
    std::map<std::string, uint64_t> inactive_call_median_ns;
};

struct HiCacheLayerWaitRemoval {
    const HiCacheLayerWaitCall * call;
    uint64_t return_us;
};

/** Remove only observed call-owned work; preserve surrounding gaps and stream order.
 * A zero return leaves a submission anchor for later dynamic expansion. */
HiCacheLayerWaitPatch append_hicache_layer_wait_removals(const HiCacheSourceDagIndex & source,
                                                        std::span<const HiCacheLayerWaitRemoval> changes, core::DagMutationPlan & plan);

/** Appends only a complete call transition plan; source observations never supply target state. */
HiCacheLayerWaitPatch append_hicache_layer_wait_plan(const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
                                                     std::vector<HiCacheRewriteDecision> & decisions, core::DagMutationPlan & plan);

} // namespace markov::trace_graph::modules::hicache::patch
