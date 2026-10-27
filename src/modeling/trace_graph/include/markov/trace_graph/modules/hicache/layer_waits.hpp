/** @file Source call sites and dependency ownership for HiCache layer waits. */
#pragma once

#include "markov/trace_graph/source_dag/index.hpp"
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace markov::trace_graph::modules::hicache {

struct HiCacheLayerWaitCall {
    std::string request_id;
    std::string phase;
    int logical_input = -1;
    uint64_t layer = 0;
    size_t position = 0;
    bool enabled = false;
    uint64_t batch_start_ns = 0;
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    std::optional<size_t> before, after, submission, worker, device_wait, record;
    source_dag::TimingIntervalOwnership cpu;
    std::string issue;
};

struct HiCacheLayerWaitObservation {
    std::string status = "unavailable";
    std::vector<HiCacheLayerWaitCall> calls;
    std::map<std::string, size_t> issues;
    std::unordered_set<size_t> cpu_node_ids;
    std::unordered_set<size_t> device_wait_node_ids;
};

/** Boundaries are observations only; this function never changes graph costs.
 * Opening-cut batch context is not admitted as a remodelable forward; its
 * selected executable tail remains in the source DAG.
 */
HiCacheLayerWaitObservation observe_hicache_layer_waits(const source_dag::SourceDagIndex & source);

} // namespace markov::trace_graph::modules::hicache
