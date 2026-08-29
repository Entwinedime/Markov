/**
 * @file
 * @brief Source/target rewrite classification and shadow-plan implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

#include "rewrite_boundary.hpp"
#include "rewrite_decision.hpp"
#include "rewrite_mutation.hpp"
#include "rewrite_normalization.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace markov::trace_graph::modules::hicache::patch {

namespace rewrite_transaction_detail {

using model::HiCacheEffectDecision;
template <typename Value> std::unordered_map<std::string, const Value *> index_by_effect_id(const std::vector<Value> & values) {
    std::unordered_map<std::string, const Value *> index;
    index.reserve(values.size());
    for (const auto & value : values) index.emplace(value.effect_id, &value);
    return index;
}

void record_ownership_conflicts(HiCacheShadowRewriteTransaction & transaction) {
    std::unordered_map<size_t, std::vector<std::string>> owners;
    for (const auto & decision : transaction.decisions) {
        if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::NoOp || decision.rewrite_kind == HiCacheRewriteKind::Reject) continue;
        for (size_t node_id : decision.owned_duration_nodes) owners[node_id].push_back(decision.effect_id);
        for (size_t node_id : decision.source_control_duration_nodes) owners[node_id].push_back(decision.effect_id);
    }
    for (auto & [node_id, effect_ids] : owners) {
        std::ranges::sort(effect_ids);
        effect_ids.erase(std::unique(effect_ids.begin(), effect_ids.end()), effect_ids.end());
        if (effect_ids.size() > 1) transaction.ownership_conflicts.emplace("node_or_gap:" + std::to_string(node_id), std::move(effect_ids));
    }
}

} // namespace rewrite_transaction_detail

uint64_t HiCacheShadowRewriteTransaction::ready_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(decisions, [](const auto & decision) { return decision.shadow_plan_ready; }));
}

uint64_t HiCacheShadowRewriteTransaction::rejected_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(decisions, [](const auto & decision) { return decision.rewrite_kind == HiCacheRewriteKind::Reject; }));
}

std::string hicache_rewrite_kind_name(HiCacheRewriteKind kind) {
    switch (kind) {
    case HiCacheRewriteKind::NoOp:
        return "no_op";
    case HiCacheRewriteKind::ReplaceWithIo:
        return "replace_with_io";
    case HiCacheRewriteKind::ReplaceWithGate:
        return "replace_with_gate";
    case HiCacheRewriteKind::RemoveOwnedCost:
        return "remove_owned_cost";
    case HiCacheRewriteKind::RemoveDependency:
        return "remove_dependency";
    case HiCacheRewriteKind::InsertIo:
        return "insert_io";
    case HiCacheRewriteKind::InsertGate:
        return "insert_gate";
    case HiCacheRewriteKind::PartialReplace:
        return "partial_replace";
    case HiCacheRewriteKind::Reject:
        return "reject";
    }
    return "unknown";
}

HiCacheShadowRewriteTransaction build_hicache_shadow_rewrite_transaction(const core::DagGraph & graph, const model::HiCacheEffectDecisionLedger & effects,
                                                                         const HiCacheSourceAttributionCatalog & attributions,
                                                                         const HiCacheIoResourcePlan & resources, bool source_target_same_config) {
    HiCacheShadowRewriteTransaction transaction;
    const auto attributions_by_id = rewrite_transaction_detail::index_by_effect_id(attributions.records);
    const auto costs_by_id = rewrite_transaction_detail::index_by_effect_id(resources.costs);
    transaction.decisions.reserve(effects.decisions.size());
    for (const auto & effect : effects.decisions) {
        const auto attribution = attributions_by_id.find(effect.effect_key);
        const auto cost = costs_by_id.find(effect.effect_key);
        auto decision = rewrite_transaction_detail::classify(effect,
                                                             attribution == attributions_by_id.end() ? nullptr : attribution->second,
                                                             cost == costs_by_id.end() ? nullptr : cost->second,
                                                             source_target_same_config);
        transaction.decisions.push_back(std::move(decision));
    }
    rewrite_transaction_detail::fold_prefetch_shift_below_polling_resolution(transaction.decisions);
    rewrite_transaction_detail::validate_completion_join_boundaries(graph, transaction.decisions);
    rewrite_transaction_detail::fold_shared_immediate_ready_completion_joins(transaction.decisions);
    rewrite_transaction_detail::resolve_target_host_control_boundaries(graph, transaction.decisions);
    rewrite_transaction_detail::fold_prefetch_visibility_into_completion_join(transaction.decisions);
    rewrite_transaction_detail::bind_background_family_consumers(transaction.decisions);
    for (const auto & decision : transaction.decisions) {
        const auto kind = hicache_rewrite_kind_name(decision.rewrite_kind);
        (void)core::checked_increment_u64(transaction.counts_by_rewrite_kind[kind], "HiCache rewrite-kind count exceeds uint64 range");
        if (!decision.blocker.empty())
            (void)core::checked_increment_u64(transaction.blocker_counts[decision.blocker], "HiCache rewrite blocker count exceeds uint64 range");
    }

    rewrite_transaction_detail::record_ownership_conflicts(transaction);
    transaction.plan = rewrite_transaction_detail::build_plan(graph, transaction.decisions, resources);
    if (!transaction.ownership_conflicts.empty()) {
        transaction.status = "ownership_conflict";
        transaction.blocker_counts["source_cost_atom_ownership_conflict"] = transaction.ownership_conflicts.size();
        return transaction;
    }
    if (!transaction.plan.empty()) {
        const auto topology = core::validate_dag_mutation_plan(graph, transaction.plan);
        transaction.topology_valid = topology.ok();
        transaction.prospective_active_node_count = topology.active_node_count;
        transaction.prospective_active_edge_count = topology.active_edge_count;
#ifdef DEBUG
        transaction.topology = topology;
#endif
        if (!topology.ok()) {
            transaction.status = "invalid_shadow_topology";
            transaction.blocker_counts["invalid_shadow_topology"] = topology.issues.size();
            return transaction;
        }
    }
    else {
        transaction.topology_valid = true;
        transaction.prospective_active_node_count = graph.active_node_count();
        transaction.prospective_active_edge_count = graph.active_edge_count();
    }

    if (transaction.rejected_count() > 0) transaction.status = transaction.ready_count() > 0 ? "partial" : "blocked";
    else transaction.status = "ready";
    return transaction;
}

} // namespace markov::trace_graph::modules::hicache::patch
