#include "rewrite_mutation.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

using model::HiCacheEffectType;

std::vector<core::DagNodeRef> resource_endpoints(const HiCacheRewriteDecision & decision,
                                                const std::unordered_map<std::string, std::string> & synthetic_by_effect,
                                                bool completion) {
    if (const auto synthetic = synthetic_by_effect.find(decision.effect_id); synthetic != synthetic_by_effect.end())
        return {core::DagNodeRef::synthetic(synthetic->second)};
    if (!decision.source_readiness_topology_reused) return {};
    const auto & nodes = completion && !decision.source_completion_node_ids.empty()
                            ? decision.source_completion_node_ids
                            : decision.owned_duration_nodes;
    std::vector<core::DagNodeRef> endpoints;
    endpoints.reserve(nodes.size());
    for (const auto node_id : nodes) endpoints.push_back(core::DagNodeRef::existing(node_id));
    return endpoints;
}

bool same_endpoint(const core::DagNodeRef & left, const core::DagNodeRef & right) {
    return left.existing_node_id == right.existing_node_id && left.synthetic_id == right.synthetic_id;
}

bool existing_edge(const core::DagGraph & graph, const core::DagNodeRef & source, const core::DagNodeRef & target) {
    if (!source.existing_node_id || !target.existing_node_id) return false;
    return std::ranges::any_of(graph.edges(), [&](const auto & edge) {
        return edge.active && edge.src == *source.existing_node_id && edge.dst == *target.existing_node_id;
    });
}

void append_resource_lane_dependencies(const core::DagGraph & graph, core::DagMutationPlan & plan, const HiCacheIoResourcePlan & resources,
                                       const std::vector<HiCacheRewriteDecision> & decisions,
                                       const std::unordered_map<std::string, std::string> & synthetic_by_effect) {
    std::map<std::string, const HiCacheRewriteDecision *> by_effect;
    for (const auto & decision : decisions)
        if (decision.shadow_plan_ready && decision.rewrite_kind != HiCacheRewriteKind::NoOp && decision.rewrite_kind != HiCacheRewriteKind::Reject)
            by_effect.emplace(decision.effect_id, &decision);
    for (const auto & dependency : resources.lane_dependencies) {
        const auto predecessor = by_effect.find(dependency.predecessor_effect_id);
        const auto successor = by_effect.find(dependency.successor_effect_id);
        if (predecessor == by_effect.end() || successor == by_effect.end()) continue;
        const auto sources = resource_endpoints(*predecessor->second, synthetic_by_effect, true);
        const auto targets = resource_endpoints(*successor->second, synthetic_by_effect, false);
        for (const auto & source : sources) {
            for (const auto & target : targets) {
                if (same_endpoint(source, target) || existing_edge(graph, source, target)) continue;
                plan.add_edges.push_back(core::DagAddEdgeMutation{
                    .src = source,
                    .dst = target,
                    .kind = core::DagEdgeKind::Mutation,
                    .effect_id = dependency.resource_lane,
                    .reason = "serialize adjacent target effects on one HiCache resource lane",
                });
            }
        }
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
                .src = core::DagNodeRef::synthetic(latest_prefetch->target_host_control_required
                                                      ? latest_prefetch->target_host_control_synthetic_id
                                                      : latest_prefetch->completion_join_required
                                                            ? latest_prefetch->completion_join_synthetic_id
                                                            : synthetic_by_effect.at(latest_prefetch->effect_id)),
                .dst = core::DagNodeRef::synthetic(decision->target_host_control_required
                                                      ? decision->target_host_control_synthetic_id
                                                      : synthetic_by_effect.at(decision->effect_id)),
                .kind = core::DagEdgeKind::Mutation,
                .effect_id = "hicache_request_io_dependency",
                .reason = "request load cannot precede its latest target prefetch boundary",
            });
        }
    }
}


} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
