#pragma once

#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

#include <unordered_map>

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

void append_resource_lane_dependencies(core::DagMutationPlan & plan, const HiCacheIoResourcePlan & resources,
                                       const std::unordered_map<std::string, std::string> & synthetic_by_effect);
void append_family_dependencies(core::DagMutationPlan & plan, const std::vector<HiCacheRewriteDecision> & decisions,
                                const std::unordered_map<std::string, std::string> & synthetic_by_effect);
void append_reused_readiness_family_dependencies(core::DagMutationPlan & plan, const std::vector<HiCacheRewriteDecision> & decisions);
void append_request_io_dependencies(core::DagMutationPlan & plan, const std::vector<HiCacheRewriteDecision> & decisions,
                                    const std::unordered_map<std::string, std::string> & synthetic_by_effect);
core::DagMutationPlan build_plan(const core::DagGraph & graph, const std::vector<HiCacheRewriteDecision> & decisions, const HiCacheIoResourcePlan & resources);

} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
