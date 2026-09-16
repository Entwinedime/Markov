/** @file Source call sites and dependency ownership for HiCache layer waits. */
#pragma once

#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
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
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    std::optional<size_t> before, after, submission, worker, device_wait, record;
    patch::HiCacheTimingIntervalOwnership cpu;
    std::string issue;
};

struct HiCacheLayerWaitObservation {
    std::string status = "unavailable";
    std::vector<HiCacheLayerWaitCall> calls;
    std::map<std::string, size_t> issues;
    std::unordered_set<size_t> cpu_node_ids;
    std::unordered_set<size_t> device_wait_node_ids;
};

/** Boundaries are observations only; this function never changes graph costs. */
HiCacheLayerWaitObservation observe_hicache_layer_waits(const patch::HiCacheSourceDagIndex & source);

} // namespace markov::trace_graph::modules::hicache
