#include "rewrite_normalization.hpp"

#include <map>
#include <ranges>
#include <set>

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

using model::HiCacheEffectType;
void fold_prefetch_visibility_into_completion_join(std::vector<HiCacheRewriteDecision> & decisions) {
    std::set<std::string> folded_families;
    for (const auto & decision : decisions) {
        if (decision.effect_type != HiCacheEffectType::PrefetchIo || !decision.shadow_plan_ready) continue;
        if (decision.rewrite_kind == HiCacheRewriteKind::NoOp || decision.completion_join_required) folded_families.insert(decision.effect_family_id);
    }
    for (auto & decision : decisions) {
        if (decision.effect_type != HiCacheEffectType::PrefetchVisibility || !folded_families.contains(decision.effect_family_id)) continue;
        decision.rewrite_kind = HiCacheRewriteKind::NoOp;
        decision.shadow_plan_ready = true;
        decision.duration_us = 0;
        decision.blocker.clear();
        decision.reason = "prefetch visibility is folded into the canonical prefetch completion boundary";
    }
}

bool synthetic_rewrite(const HiCacheRewriteDecision & decision) {
    return decision.shadow_plan_ready && decision.rewrite_kind != HiCacheRewriteKind::NoOp && decision.rewrite_kind != HiCacheRewriteKind::Reject
           && decision.rewrite_kind != HiCacheRewriteKind::RemoveOwnedCost && decision.rewrite_kind != HiCacheRewriteKind::RemoveDependency;
}

bool materialized_rewrite(const HiCacheRewriteDecision & decision) { return decision.source_readiness_topology_reused || synthetic_rewrite(decision); }

void bind_background_family_consumers(std::vector<HiCacheRewriteDecision> & decisions) {
    using Family = std::map<HiCacheEffectType, HiCacheRewriteDecision *>;
    std::map<std::string, Family> families;
    for (auto & decision : decisions) {
        if (!decision.effect_family_id.empty()) families[decision.effect_family_id][decision.effect_type] = &decision;
    }
    const auto bind = [&](Family & family, HiCacheEffectType effect_type, HiCacheEffectType successor_type) {
        const auto effect = family.find(effect_type);
        if (effect == family.end() || !materialized_rewrite(*effect->second) || !effect->second->consumer_anchors.empty()) return;
        const auto successor = family.find(successor_type);
        if (successor != family.end() && synthetic_rewrite(*successor->second)) {
            effect->second->family_consumer_synthetic_id = successor->second->synthetic_id;
            return;
        }
        effect->second->reason += "; no foreground family consumer is materialized, so the operation remains resource-only";
    };
    for (auto & family : families | std::views::values) { bind(family, HiCacheEffectType::CommitDeviceToHost, HiCacheEffectType::CommitHostToStorage); }
}


} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
