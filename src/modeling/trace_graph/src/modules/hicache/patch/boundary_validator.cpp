/**
 * @file
 * @brief Effect-local boundary and no-double-count validation implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/boundary_validator.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch {

namespace boundary_validator_detail {

bool ref_is_existing(const core::DagNodeRef & ref, size_t node_id) { return ref.existing_node_id && *ref.existing_node_id == node_id; }

bool ref_is_synthetic(const core::DagNodeRef & ref, std::string_view synthetic_id) { return ref.synthetic_id == synthetic_id; }

bool has_added_edge(const core::DagMutationPlan & plan, const core::DagNodeRef & src, const core::DagNodeRef & dst, std::string_view effect_id) {
    return std::ranges::any_of(plan.add_edges, [&](const auto & edge) {
        const bool source_matches = src.existing_node_id ? ref_is_existing(edge.src, *src.existing_node_id) : ref_is_synthetic(edge.src, src.synthetic_id);
        const bool destination_matches = dst.existing_node_id ? ref_is_existing(edge.dst, *dst.existing_node_id) : ref_is_synthetic(edge.dst, dst.synthetic_id);
        return source_matches && destination_matches && edge.effect_id == effect_id;
    });
}

bool duration_removed(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    return std::ranges::all_of(decision.owned_duration_nodes, [&](size_t node_id) {
        return std::ranges::count_if(
                   plan.set_node_durations,
                   [&](const auto & update) { return update.node_id == node_id && update.duration == 0 && update.effect_id == decision.effect_id; })
               == 1;
    });
}

const core::DagSyntheticNodeMutation * synthetic_node(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    const core::DagSyntheticNodeMutation * match = nullptr;
    for (const auto & node : plan.synthetic_nodes) {
        if (node.synthetic_id != decision.synthetic_id || node.effect_id != decision.effect_id) continue;
        if (match != nullptr) return nullptr;
        match = &node;
    }
    return match;
}

bool source_boundary_untouched(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    for (size_t node_id : decision.carrier_nodes) {
        if (std::ranges::find(plan.disable_nodes, node_id) != plan.disable_nodes.end()) return false;
    }
    for (size_t edge_index : decision.carrier_exit_edges) {
        if (std::ranges::find(plan.disable_edges, edge_index) != plan.disable_edges.end()) return false;
        if (std::ranges::any_of(plan.redirect_edges, [&](const auto & redirect) { return redirect.edge_index == edge_index; })) return false;
    }
    return true;
}

const core::DagRedirectEdgeMutation * redirected_ingress(const core::DagMutationPlan & plan, size_t edge_index, const HiCacheRewriteDecision & decision) {
    const core::DagRedirectEdgeMutation * match = nullptr;
    for (const auto & redirect : plan.redirect_edges) {
        if (redirect.edge_index != edge_index || redirect.effect_id != decision.effect_id) continue;
        if (match != nullptr) return nullptr;
        match = &redirect;
    }
    return match;
}

bool replacement_ingress(const core::DagGraph & graph, const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (decision.carrier_entry_edges.empty()) {
        return !decision.carrier_nodes.empty()
               && has_added_edge(plan,
                                 core::DagNodeRef::synthetic(decision.synthetic_id),
                                 core::DagNodeRef::existing(decision.carrier_nodes.front()),
                                 decision.effect_id);
    }
    for (size_t edge_index : decision.carrier_entry_edges) {
        if (edge_index >= graph.edge_count() || !graph.edge(edge_index).active) return false;
        const auto & edge = graph.edge(edge_index);
        const auto * redirect = redirected_ingress(plan, edge_index, decision);
        if (redirect == nullptr || !redirect->dst || !ref_is_synthetic(*redirect->dst, decision.synthetic_id)) return false;
        if (!has_added_edge(plan, core::DagNodeRef::synthetic(decision.synthetic_id), core::DagNodeRef::existing(edge.dst), decision.effect_id)) return false;
    }
    return true;
}

bool insertion_dependencies(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!has_added_edge(plan, core::DagNodeRef::existing(decision.source_fact_node_id), core::DagNodeRef::synthetic(decision.synthetic_id), decision.effect_id))
        return false;
    return !decision.consumer_anchors.empty() && std::ranges::all_of(decision.consumer_anchors, [&](size_t consumer) {
        return has_added_edge(plan, core::DagNodeRef::synthetic(decision.synthetic_id), core::DagNodeRef::existing(consumer), decision.effect_id);
    });
}

HiCacheBoundaryValidation validate_one(const core::DagGraph & graph, const HiCacheShadowRewriteTransaction & shadow, const HiCacheRewriteDecision & decision) {
    HiCacheBoundaryValidation validation{
        .effect_id = decision.effect_id,
        .rewrite_kind = decision.rewrite_kind,
    };
    if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::Reject) {
        validation.reason = decision.blocker.empty() ? "rewrite is not shadow-plan ready" : decision.blocker;
        return validation;
    }
    if (decision.rewrite_kind == HiCacheRewriteKind::NoOp) {
        validation.ready = true;
        validation.source_cost_removed = true;
        validation.target_cost_materialized = true;
        validation.ingress_preserved = true;
        validation.egress_preserved = true;
        validation.consumer_dependency_ready = true;
        validation.reason = "source and target both omit the effect";
        return validation;
    }

    const bool insertion = decision.rewrite_kind == HiCacheRewriteKind::InsertIo || decision.rewrite_kind == HiCacheRewriteKind::InsertGate;
    const bool removal = decision.rewrite_kind == HiCacheRewriteKind::RemoveOwnedCost || decision.rewrite_kind == HiCacheRewriteKind::RemoveDependency;
    const bool gate = decision.rewrite_kind == HiCacheRewriteKind::ReplaceWithGate || decision.rewrite_kind == HiCacheRewriteKind::InsertGate;
    validation.source_cost_removed = insertion || gate || duration_removed(shadow.plan, decision);
    validation.egress_preserved = insertion || source_boundary_untouched(shadow.plan, decision);
    if (removal) {
        validation.target_cost_materialized = true;
        validation.ingress_preserved = source_boundary_untouched(shadow.plan, decision);
        validation.consumer_dependency_ready = validation.egress_preserved;
    }
    else {
        const auto * synthetic = synthetic_node(shadow.plan, decision);
        validation.target_cost_materialized = synthetic != nullptr && synthetic->node.duration == decision.duration_us;
        validation.ingress_preserved = (insertion || gate) ? insertion_dependencies(shadow.plan, decision) : replacement_ingress(graph, shadow.plan, decision);
        validation.consumer_dependency_ready = insertion ? validation.ingress_preserved : validation.egress_preserved;
    }
    validation.ready = validation.source_cost_removed && validation.target_cost_materialized && validation.ingress_preserved && validation.egress_preserved
                       && validation.consumer_dependency_ready;
    validation.reason = validation.ready ? "effect-local source cost, target cost, and retained boundaries are consistent"
                                         : "effect-local shadow plan failed a boundary or no-double-count invariant";
    return validation;
}

} // namespace boundary_validator_detail

uint64_t HiCacheBoundaryValidationCatalog::ready_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(records, [](const auto & record) { return record.ready; }));
}

HiCacheBoundaryValidationCatalog validate_hicache_shadow_boundaries(const core::DagGraph & graph, const HiCacheShadowRewriteTransaction & shadow) {
    HiCacheBoundaryValidationCatalog catalog;
    catalog.records.reserve(shadow.decisions.size());
    for (const auto & decision : shadow.decisions) {
        auto record = boundary_validator_detail::validate_one(graph, shadow, decision);
        if (!record.ready) (void)core::checked_increment_u64(catalog.blocker_counts[record.reason], "HiCache boundary blocker count exceeds uint64 range");
        catalog.records.push_back(std::move(record));
    }
    if (catalog.records.empty()) catalog.status = "no_rewrite_decisions";
    else if (catalog.ready_count() == catalog.records.size()) catalog.status = "ready";
    else if (catalog.ready_count() > 0) catalog.status = "partial";
    else catalog.status = "blocked";
    return catalog;
}

} // namespace markov::trace_graph::modules::hicache::patch
