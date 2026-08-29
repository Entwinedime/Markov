#pragma once

#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

void fold_prefetch_shift_below_polling_resolution(std::vector<HiCacheRewriteDecision> & decisions);
void fold_prefetch_visibility_into_completion_join(std::vector<HiCacheRewriteDecision> & decisions);
void bind_background_family_consumers(std::vector<HiCacheRewriteDecision> & decisions);

} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
