#pragma once
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

namespace markov::trace_graph::modules::hicache::patch {

struct HiCacheLayerTransferTemplate {
    uint64_t layer = 0, bytes = 0, record_us = 0;
    size_t source_record = 0;
};

/** Byte-weighted layer layout from one complete source loadback and its Record/WAIT edges. */
std::vector<HiCacheLayerTransferTemplate> observe_hicache_layer_transfers(const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
                                                                          const HiCacheRewriteDecision & donor);

/** Expand aggregate service, preserving its ingress and moving resource egress to the final layer. */
bool expand_hicache_layer_io(const std::vector<HiCacheLayerTransferTemplate> & layers, HiCacheRewriteDecision & decision, core::DagMutationPlan & plan);

} // namespace markov::trace_graph::modules::hicache::patch
