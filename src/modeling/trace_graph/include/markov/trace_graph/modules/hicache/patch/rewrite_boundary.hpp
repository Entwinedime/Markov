#pragma once

#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

void validate_completion_join_boundaries(const core::DagGraph & graph, std::vector<HiCacheRewriteDecision> & decisions);
void fold_shared_immediate_ready_completion_joins(std::vector<HiCacheRewriteDecision> & decisions);
void resolve_target_host_control_boundaries(const core::DagGraph & graph, std::vector<HiCacheRewriteDecision> & decisions);

} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
