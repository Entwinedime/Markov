#pragma once

#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

HiCacheRewriteDecision classify(const model::HiCacheEffectDecision & effect, const HiCacheSourceAttribution * attribution, const HiCacheIoCostRecord * cost,
                                bool source_target_same_config);

} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
