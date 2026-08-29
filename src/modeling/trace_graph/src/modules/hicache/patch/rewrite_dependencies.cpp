#include "rewrite_mutation.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

using model::HiCacheEffectType;

void append_resource_lane_dependencies(core::DagMutationPlan & plan, const HiCacheIoResourcePlan & resources,
                                       const std::unordered_map<std::string, std::string> & synthetic_by_effect) {
    for (const auto & dependency : resources.lane_dependencies) {
        const auto predecessor = synthetic_by_effect.find(dependency.predecessor_effect_id);
        const auto successor = synthetic_by_effect.find(dependency.successor_effect_id);
        if (predecessor == synthetic_by_effect.end() || successor == synthetic_by_effect.end()) continue;
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(predecessor->second),
            .dst = core::DagNodeRef::synthetic(successor->second),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = dependency.resource_lane,
            .reason = "serialize adjacent target effects on one HiCache resource lane",
        });
    }
}

void append_family_dependencies(core::DagMutationPlan & plan, const std::vector<HiCacheRewriteDecision> & decisions,
                                const std::unordered_map<std::string, std::string> & synthetic_by_effect) {
    using FamilyEndpoints = std::map<HiCacheEffectType, std::string>;
    std::map<std::string, FamilyEndpoints> endpoints;
    for (const auto & decision : decisions) {
        const auto synthetic = synthetic_by_effect.find(decision.effect_id);
        if (decision.effect_family_id.empty() || synthetic == synthetic_by_effect.end()) continue;
        endpoints[decision.effect_family_id].emplace(decision.effect_type, synthetic->second);
    }
    const auto append = [&](const FamilyEndpoints & family, HiCacheEffectType predecessor_type, HiCacheEffectType successor_type, std::string_view reason) {
        const auto predecessor = family.find(predecessor_type);
        const auto successor = family.find(successor_type);
        if (predecessor == family.end() || successor == family.end()) return;
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(predecessor->second),
            .dst = core::DagNodeRef::synthetic(successor->second),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = "hicache_family_dependency",
            .reason = std::string(reason),
        });
    };
    for (const auto & family : endpoints | std::views::values) {
        append(family, HiCacheEffectType::PrefetchIo, HiCacheEffectType::PrefetchVisibility, "prefetch visibility cannot precede its target storage operation");
        append(family,
               HiCacheEffectType::CommitDeviceToHost,
               HiCacheEffectType::CommitHostToStorage,
               "host-to-storage commit cannot precede target device-to-host materialization");
        append(family,
               HiCacheEffectType::CommitDeviceToHost,
               HiCacheEffectType::CommitCapacityGate,
               "a proven blocking write-back capacity release cannot precede target device-to-host completion");
    }
}

void append_reused_readiness_family_dependencies(core::DagMutationPlan & plan, const std::vector<HiCacheRewriteDecision> & decisions) {
    for (const auto & decision : decisions) {
        if (!decision.source_readiness_topology_reused || decision.family_consumer_synthetic_id.empty()) continue;
        const auto & endpoints = decision.source_completion_node_ids.empty() ? decision.owned_duration_nodes : decision.source_completion_node_ids;
        for (size_t endpoint : endpoints) {
            plan.add_edges.push_back(core::DagAddEdgeMutation{
                .src = core::DagNodeRef::existing(endpoint),
                .dst = core::DagNodeRef::synthetic(decision.family_consumer_synthetic_id),
                .kind = core::DagEdgeKind::Mutation,
                .effect_id = "hicache_family_dependency",
                .reason = "target background successor waits for the carried source device-transfer completion topology",
            });
        }
    }
}

void append_request_io_dependencies(core::DagMutationPlan & plan, const std::vector<HiCacheRewriteDecision> & decisions,
                                    const std::unordered_map<std::string, std::string> & synthetic_by_effect) {
    std::map<std::string, std::vector<const HiCacheRewriteDecision *>> by_request;
    for (const auto & decision : decisions) {
        if (decision.request_id.empty() || !synthetic_by_effect.contains(decision.effect_id)) continue;
        if (decision.effect_type != HiCacheEffectType::PrefetchIo && decision.effect_type != HiCacheEffectType::Loadback) continue;
        by_request[decision.cache_scope + "\x1f" + decision.request_id].push_back(&decision);
    }
    for (auto & request : by_request | std::views::values) {
        std::ranges::sort(request, [](const auto * left, const auto * right) {
            if (left->eligibility_timestamp_us != right->eligibility_timestamp_us) return left->eligibility_timestamp_us < right->eligibility_timestamp_us;
            return left->effect_id < right->effect_id;
        });
        const HiCacheRewriteDecision * latest_prefetch = nullptr;
        for (const auto * decision : request) {
            if (decision->effect_type == HiCacheEffectType::PrefetchIo) {
                latest_prefetch = decision;
                continue;
            }
            if (latest_prefetch == nullptr) continue;
            plan.add_edges.push_back(core::DagAddEdgeMutation{
                .src = core::DagNodeRef::synthetic(synthetic_by_effect.at(latest_prefetch->effect_id)),
                .dst = core::DagNodeRef::synthetic(synthetic_by_effect.at(decision->effect_id)),
                .kind = core::DagEdgeKind::Mutation,
                .effect_id = "hicache_request_io_dependency",
                .reason = "request load cannot precede its latest target prefetch boundary",
            });
        }
    }
}


} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
