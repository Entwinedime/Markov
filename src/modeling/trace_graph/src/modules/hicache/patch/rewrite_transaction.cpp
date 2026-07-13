/**
 * @file
 * @brief Source/target rewrite classification and shadow-plan implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <unordered_map>

namespace markov::trace_graph::modules::hicache::patch {

namespace rewrite_transaction_detail {

using model::HiCacheEffectDecision;
using model::HiCacheEffectType;
using model::HiCacheSourceCarrierState;
using model::HiCacheTargetEffectState;
using model::HiCacheTransferDirection;

template <typename Value> std::unordered_map<std::string, const Value *> index_by_effect_id(const std::vector<Value> & values) {
    std::unordered_map<std::string, const Value *> index;
    index.reserve(values.size());
    for (const auto & value : values) index.emplace(value.effect_id, &value);
    return index;
}

bool dependency_effect(const HiCacheEffectDecision & effect) { return effect.direction == HiCacheTransferDirection::None; }

HiCacheRewriteDecision reject_decision(const HiCacheEffectDecision & effect, const HiCacheSourceAttribution * attribution, std::string blocker) {
    return HiCacheRewriteDecision{
        .effect_id = effect.effect_key,
        .effect_family_id = effect.effect_family_key,
        .effect_type = effect.effect_type,
        .target_effect_state = effect.target_effect_state,
        .source_carrier_state = attribution == nullptr ? HiCacheSourceCarrierState::NotEvaluated : attribution->source_carrier_state,
        .rewrite_kind = HiCacheRewriteKind::Reject,
        .source_fact_node_id = effect.source_node_id,
        .reason = "rewrite is not safe under the current source/target evidence",
        .blocker = std::move(blocker),
    };
}

HiCacheRewriteDecision classify(const HiCacheEffectDecision & effect, const HiCacheSourceAttribution * attribution, const HiCacheIoCostRecord * cost) {
    if (attribution == nullptr) return reject_decision(effect, nullptr, "missing_source_attribution");
    if (effect.target_effect_state == HiCacheTargetEffectState::Deferred) return reject_decision(effect, attribution, "target_effect_deferred");
    if (effect.target_effect_state == HiCacheTargetEffectState::Unresolved) return reject_decision(effect, attribution, "target_effect_unresolved");
    if (attribution->source_carrier_state == HiCacheSourceCarrierState::NotEvaluated)
        return reject_decision(effect, attribution, "source_carrier_not_evaluated");
    if (attribution->source_carrier_state == HiCacheSourceCarrierState::Unobservable)
        return reject_decision(effect, attribution, "source_carrier_unobservable:" + attribution->reason);
    if (attribution->source_carrier_state == HiCacheSourceCarrierState::Ambiguous)
        return reject_decision(effect, attribution, "source_carrier_ambiguous:" + attribution->reason);

    HiCacheRewriteDecision decision{
        .effect_id = effect.effect_key,
        .effect_family_id = effect.effect_family_key,
        .effect_type = effect.effect_type,
        .target_effect_state = effect.target_effect_state,
        .source_carrier_state = attribution->source_carrier_state,
        .duration_us = cost == nullptr ? 0 : cost->duration_us,
        .resource_lane = cost == nullptr ? std::string{} : cost->resource_lane,
        .synthetic_id = "hicache_effect:" + effect.effect_key,
        .carrier_nodes = attribution->carrier_nodes,
        .owned_duration_nodes = attribution->owned_duration_nodes,
        .carrier_entry_edges = attribution->carrier_entry_edges,
        .carrier_exit_edges = attribution->carrier_exit_edges,
        .source_fact_node_id = effect.source_node_id,
        .consumer_anchors = attribution->consumer_anchors,
        .consumer_anchor_method = attribution->consumer_anchor_method,
    };

    if (attribution->source_carrier_state == HiCacheSourceCarrierState::Absent) {
        if (effect.target_effect_state == HiCacheTargetEffectState::NotRequired) {
            decision.rewrite_kind = HiCacheRewriteKind::NoOp;
            decision.shadow_plan_ready = true;
            decision.reason = "source and target both explicitly omit this effect";
            return decision;
        }
        if (cost == nullptr || cost->status != HiCacheIoCostStatus::Ready) return reject_decision(effect, attribution, "target_effect_cost_not_ready");
        if (attribution->consumer_anchors.empty()) return reject_decision(effect, attribution, "missing_insertion_consumer_anchor");
        decision.rewrite_kind = dependency_effect(effect) ? HiCacheRewriteKind::InsertGate : HiCacheRewriteKind::InsertIo;
        decision.shadow_plan_ready = true;
        decision.reason = "target requires an effect absent from the source DAG";
        return decision;
    }

    if (effect.target_effect_state == HiCacheTargetEffectState::NotRequired) {
        if (dependency_effect(effect)) {
            decision.rewrite_kind = HiCacheRewriteKind::RemoveDependency;
            decision.shadow_plan_ready = true;
            decision.reason = "target omits the source control dependency without claiming scheduler CPU cost";
            return decision;
        }
        if (attribution->owned_duration_nodes.empty()) return reject_decision(effect, attribution, "present_source_carrier_has_no_owned_duration");
        decision.rewrite_kind = HiCacheRewriteKind::RemoveOwnedCost;
        decision.shadow_plan_ready = true;
        decision.reason = "target explicitly removes the source effect while retaining its boundary";
        return decision;
    }
    if (cost == nullptr || cost->status != HiCacheIoCostStatus::Ready) return reject_decision(effect, attribution, "target_effect_cost_not_ready");
    if (effect.target_effect_state == HiCacheTargetEffectState::Partial) {
        if (dependency_effect(effect)) return reject_decision(effect, attribution, "partial_dependency_effect_is_invalid");
        if (attribution->owned_duration_nodes.empty()) return reject_decision(effect, attribution, "present_source_carrier_has_no_owned_duration");
        decision.rewrite_kind = HiCacheRewriteKind::PartialReplace;
        decision.shadow_plan_ready = true;
        decision.reason = "source timing is effect-local and can be replaced completely by the target-derived partial transfer cost";
        return decision;
    }
    if (dependency_effect(effect)) {
        if (attribution->consumer_anchors.empty()) return reject_decision(effect, attribution, "missing_dependency_consumer_anchor");
        decision.rewrite_kind = HiCacheRewriteKind::ReplaceWithGate;
    }
    else {
        if (attribution->owned_duration_nodes.empty()) return reject_decision(effect, attribution, "present_source_carrier_has_no_owned_duration");
        decision.rewrite_kind = HiCacheRewriteKind::ReplaceWithIo;
    }
    decision.shadow_plan_ready = true;
    decision.reason = "source carrier can be replaced by the target-derived effect cost";
    return decision;
}

void record_ownership_conflicts(HiCacheShadowRewriteTransaction & transaction) {
    std::unordered_map<size_t, std::vector<std::string>> owners;
    for (const auto & decision : transaction.decisions) {
        if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::NoOp || decision.rewrite_kind == HiCacheRewriteKind::Reject) continue;
        for (size_t node_id : decision.owned_duration_nodes) owners[node_id].push_back(decision.effect_id);
    }
    for (auto & [node_id, effect_ids] : owners) {
        std::ranges::sort(effect_ids);
        effect_ids.erase(std::unique(effect_ids.begin(), effect_ids.end()), effect_ids.end());
        if (effect_ids.size() > 1) transaction.ownership_conflicts.emplace("node_duration:" + std::to_string(node_id), std::move(effect_ids));
    }
}

void append_duration_updates(core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    for (size_t node_id : decision.owned_duration_nodes) {
        plan.set_node_durations.push_back(core::DagSetNodeDurationMutation{
            .node_id = node_id,
            .duration = 0,
            .effect_id = decision.effect_id,
            .reason = "remove source-owned HiCache duration while retaining boundary identity and CPU gap",
        });
    }
}

void append_synthetic_node(core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    plan.synthetic_nodes.push_back(core::DagSyntheticNodeMutation{
        .synthetic_id = decision.synthetic_id,
        .node = core::DagSyntheticNodeSpec{
            .name = "hicache_" + model::hicache_effect_type_name(decision.effect_type),
            .category = "hicache_patch",
            .is_cpu = false,
            .lane_key = decision.resource_lane.empty() ? "hicache_dependency" : decision.resource_lane,
            .duration = decision.duration_us,
            .attrs = {
                { "effect_id", decision.effect_id },
                { "rewrite_kind", hicache_rewrite_kind_name(decision.rewrite_kind) },
            },
        },
        .effect_id = decision.effect_id,
        .reason = decision.reason,
    });
}

void append_replacement_edges(const core::DagGraph & graph, core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    std::set<size_t> boundary_nodes;
    for (size_t edge_index : decision.carrier_entry_edges) {
        const auto & edge = graph.edge(edge_index);
        plan.redirect_edges.push_back(core::DagRedirectEdgeMutation{
            .edge_index = edge_index,
            .dst = core::DagNodeRef::synthetic(decision.synthetic_id),
            .effect_id = decision.effect_id,
            .reason = "route the proven source-carrier ingress through the target effect",
        });
        boundary_nodes.insert(edge.dst);
    }
    if (boundary_nodes.empty() && !decision.carrier_nodes.empty()) boundary_nodes.insert(decision.carrier_nodes.front());
    for (size_t node_id : boundary_nodes) {
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.synthetic_id),
            .dst = core::DagNodeRef::existing(node_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "target effect completes before the retained zero-cost source boundary",
        });
    }
}

void append_insertion_edges(core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    plan.add_edges.push_back(core::DagAddEdgeMutation{
        .src = core::DagNodeRef::existing(decision.source_fact_node_id),
        .dst = core::DagNodeRef::synthetic(decision.synthetic_id),
        .kind = core::DagEdgeKind::Mutation,
        .effect_id = decision.effect_id,
        .reason = "target effect becomes eligible after its source opportunity anchor",
    });
    for (size_t consumer : decision.consumer_anchors) {
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.synthetic_id),
            .dst = core::DagNodeRef::existing(consumer),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "target consumer waits for inserted HiCache effect",
        });
    }
}

void append_gate_edges(core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) { append_insertion_edges(plan, decision); }

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
               HiCacheEffectType::CommitHostToStorage,
               HiCacheEffectType::CommitCapacityGate,
               "commit capacity release cannot precede target storage completion");
    }
}

core::DagMutationPlan build_plan(const core::DagGraph & graph, const std::vector<HiCacheRewriteDecision> & decisions, const HiCacheIoResourcePlan & resources) {
    core::DagMutationPlan plan{
        .plan_id = "hicache_shadow_rewrite_transaction",
        .reason = "read-only prospective HiCache rewrite transaction",
    };
    std::unordered_map<std::string, std::string> synthetic_by_effect;
    for (const auto & decision : decisions) {
        if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::NoOp || decision.rewrite_kind == HiCacheRewriteKind::Reject) continue;
        if (decision.rewrite_kind == HiCacheRewriteKind::RemoveOwnedCost || decision.rewrite_kind == HiCacheRewriteKind::RemoveDependency) {
            append_duration_updates(plan, decision);
            continue;
        }
        append_synthetic_node(plan, decision);
        synthetic_by_effect.emplace(decision.effect_id, decision.synthetic_id);
        if (decision.rewrite_kind == HiCacheRewriteKind::ReplaceWithGate || decision.rewrite_kind == HiCacheRewriteKind::InsertGate) {
            append_gate_edges(plan, decision);
            continue;
        }
        if (decision.source_carrier_state == HiCacheSourceCarrierState::Present) {
            append_duration_updates(plan, decision);
            append_replacement_edges(graph, plan, decision);
        }
        else append_insertion_edges(plan, decision);
    }
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
    append_family_dependencies(plan, decisions, synthetic_by_effect);
    return plan;
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
                                                                         const HiCacheIoResourcePlan & resources) {
    HiCacheShadowRewriteTransaction transaction;
    transaction.io_model_calibration_status = resources.io_model_calibration_status;
    transaction.io_model_allows_apply = resources.calibrated_for_apply();
    const auto attributions_by_id = rewrite_transaction_detail::index_by_effect_id(attributions.records);
    const auto costs_by_id = rewrite_transaction_detail::index_by_effect_id(resources.costs);
    transaction.decisions.reserve(effects.decisions.size());
    for (const auto & effect : effects.decisions) {
        const auto attribution = attributions_by_id.find(effect.effect_key);
        const auto cost = costs_by_id.find(effect.effect_key);
        auto decision = rewrite_transaction_detail::classify(effect,
                                                             attribution == attributions_by_id.end() ? nullptr : attribution->second,
                                                             cost == costs_by_id.end() ? nullptr : cost->second);
        const auto kind = hicache_rewrite_kind_name(decision.rewrite_kind);
        (void)core::checked_increment_u64(transaction.counts_by_rewrite_kind[kind], "HiCache rewrite-kind count exceeds uint64 range");
        if (!decision.blocker.empty())
            (void)core::checked_increment_u64(transaction.blocker_counts[decision.blocker], "HiCache rewrite blocker count exceeds uint64 range");
        transaction.decisions.push_back(std::move(decision));
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
