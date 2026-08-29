/**
 * @file
 * @brief Effect-local boundary and no-double-count validation implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/boundary_validator.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
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

bool duration_removed(const core::DagGraph & graph, const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    const bool nodes_removed = std::ranges::all_of(decision.owned_duration_nodes, [&](size_t node_id) {
        return std::ranges::count_if(
                   plan.set_node_durations,
                   [&](const auto & update) { return update.node_id == node_id && update.duration == 0 && update.effect_id == decision.effect_id; })
               == 1;
    });
    std::set<size_t> owned_gap_nodes;
    const auto append_gap = [&](const HiCacheCpuGapSlice & gap) { owned_gap_nodes.insert(gap.owner_node_id); };
    if (decision.completion_join_required) {
        for (const auto & gap : decision.completion_wait_slices) append_gap(gap);
        for (const auto & gap : decision.logical_input_completion_wait_slices) append_gap(gap);
    }
    else {
        for (const auto & gap : decision.owned_gap_slices) append_gap(gap);
        for (const auto & gap : decision.logical_input_causal_gap_slices) append_gap(gap);
    }
    const bool gaps_removed = std::ranges::all_of(owned_gap_nodes, [&](size_t node_id) {
        if (node_id >= graph.node_count()) return false;
        return std::ranges::count_if(plan.set_cpu_gaps, [&](const auto & update) { return update.node_id == node_id; }) == 1;
    });
    return nodes_removed && gaps_removed;
}

bool readiness_topology_duration_replaced(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.source_readiness_topology_reused || decision.owned_duration_nodes.empty()) return false;
    uint64_t materialized_duration = 0;
    for (size_t node_id : decision.owned_duration_nodes) {
        const core::DagSetNodeDurationMutation * match = nullptr;
        for (const auto & update : plan.set_node_durations) {
            if (update.node_id != node_id || update.effect_id != decision.effect_id) continue;
            if (match != nullptr) return false;
            match = &update;
        }
        if (match == nullptr) return false;
        materialized_duration =
            core::checked_add_u64(materialized_duration, match->duration, "HiCache readiness-topology validation duration exceeds uint64 range");
    }
    return materialized_duration == decision.duration_us;
}

bool source_control_removed(const core::DagGraph & graph, const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.source_control_removal_required) return true;
    if (decision.source_control_duration_nodes.empty() && decision.source_control_gap_slices.empty()) return false;
    const bool nodes_removed = std::ranges::all_of(decision.source_control_duration_nodes, [&](size_t node_id) {
        const core::DagSetNodeDurationMutation * match = nullptr;
        for (const auto & update : plan.set_node_durations) {
            if (update.node_id != node_id || update.effect_id != decision.effect_id) continue;
            if (match != nullptr) return false;
            match = &update;
        }
        return match != nullptr && match->duration == 0;
    });
    std::set<size_t> gap_nodes;
    for (const auto & gap : decision.source_control_gap_slices) gap_nodes.insert(gap.owner_node_id);
    const bool gaps_removed = std::ranges::all_of(gap_nodes, [&](size_t node_id) {
        return node_id < graph.node_count() && std::ranges::count_if(plan.set_cpu_gaps, [&](const auto & update) { return update.node_id == node_id; }) == 1;
    });
    return nodes_removed && gaps_removed;
}

const core::DagSyntheticNodeMutation * completion_join_node(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    const core::DagSyntheticNodeMutation * match = nullptr;
    for (const auto & node : plan.synthetic_nodes) {
        if (node.synthetic_id != decision.completion_join_synthetic_id || node.effect_id != decision.effect_id) continue;
        if (match != nullptr) return nullptr;
        match = &node;
    }
    return match;
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

const core::DagSyntheticNodeMutation * target_host_control_node(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    const core::DagSyntheticNodeMutation * match = nullptr;
    for (const auto & node : plan.synthetic_nodes) {
        if (node.synthetic_id != decision.target_host_control_synthetic_id || node.effect_id != decision.effect_id) continue;
        if (match != nullptr) return nullptr;
        match = &node;
    }
    return match;
}

core::DagNodeRef target_effect_ingress(const HiCacheRewriteDecision & decision) {
    if (decision.target_host_control_required && !decision.target_host_control_terminal)
        return core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id);
    if (!decision.source_execution_anchor_node_id) return {};
    return core::DagNodeRef::existing(*decision.source_execution_anchor_node_id);
}

bool target_host_control_materialized(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    const auto * control = target_host_control_node(plan, decision);
    if (!decision.target_host_control_required) return control == nullptr;
    if (control == nullptr || !control->node.is_cpu || control->node.counts_toward_e2e || control->node.duration != decision.target_host_control_duration_us
        || (decision.target_host_control_exit_node_ids.empty() && decision.effect_type != model::HiCacheEffectType::CommitDeviceToHost))
        return false;
    if (decision.target_host_control_terminal) {
        if (decision.completion_join_required || decision.target_host_control_exit_node_ids.size() != 1
            || decision.target_host_control_ingress_edge_ids.empty())
            return false;
        const auto control_ref = core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id);
        const auto join_ref = core::DagNodeRef::synthetic(decision.target_host_control_terminal_join_synthetic_id);
        const auto exit_ref = core::DagNodeRef::existing(decision.target_host_control_exit_node_ids.front());
        const auto join_count = std::ranges::count_if(plan.synthetic_nodes, [&](const auto & node) {
            return node.synthetic_id == decision.target_host_control_terminal_join_synthetic_id && node.effect_id == decision.effect_id
                   && node.node.duration == 0 && !node.node.counts_toward_e2e;
        });
        if (join_count != 1 || !has_added_edge(plan, join_ref, control_ref, decision.effect_id)
            || !has_added_edge(plan, control_ref, exit_ref, decision.effect_id))
            return false;
        return std::ranges::all_of(decision.target_host_control_ingress_edge_ids, [&](size_t edge_id) {
            const core::DagRedirectEdgeMutation * match = nullptr;
            for (const auto & redirect : plan.redirect_edges) {
                if (redirect.edge_index != edge_id || redirect.effect_id != decision.effect_id) continue;
                if (match != nullptr) return false;
                match = &redirect;
            }
            return match != nullptr && match->dst && ref_is_synthetic(*match->dst, decision.target_host_control_terminal_join_synthetic_id);
        });
    }
    if (!decision.target_host_control_anchor_node_id) return false;
    if (!std::ranges::all_of(decision.target_host_control_exit_node_ids, [&](size_t exit_node_id) {
            return has_added_edge(plan,
                                  core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
                                  core::DagNodeRef::existing(exit_node_id),
                                  decision.effect_id);
        }))
        return false;
    if (!has_added_edge(plan,
                        core::DagNodeRef::existing(*decision.target_host_control_anchor_node_id),
                        core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
                        decision.effect_id))
        return false;
    if (!decision.source_readiness_topology_reused) {
        return has_added_edge(plan,
                              core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
                              core::DagNodeRef::synthetic(decision.synthetic_id),
                              decision.effect_id);
    }
    return !decision.owned_duration_nodes.empty() && std::ranges::all_of(decision.owned_duration_nodes, [&](size_t node_id) {
        return has_added_edge(plan,
                              core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
                              core::DagNodeRef::existing(node_id),
                              decision.effect_id);
    });
}

bool endpoint_update_present(const core::DagMutationPlan & plan, size_t node_id) {
    return std::ranges::count_if(plan.set_node_e2e_eligibility, [&](const auto & update) { return update.node_id == node_id && !update.counts_toward_e2e; })
           == 1;
}

bool observable_endpoint_contract(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    const auto infrastructure_ready = [&](const auto & nodes) {
        return std::ranges::all_of(nodes, [&](size_t node_id) { return endpoint_update_present(plan, node_id); });
    };
    if (!infrastructure_ready(decision.owned_duration_nodes) || !infrastructure_ready(decision.source_completion_node_ids)
        || !infrastructure_ready(decision.readiness_join_node_ids) || !infrastructure_ready(decision.completion_wait_owned_node_ids))
        return false;
    const auto * effect = synthetic_node(plan, decision);
    if (effect != nullptr && effect->node.counts_toward_e2e) return false;
    const auto * host_control = target_host_control_node(plan, decision);
    if (host_control != nullptr && host_control->node.counts_toward_e2e) return false;
    const auto * join = completion_join_node(plan, decision);
    return join == nullptr || !join->node.counts_toward_e2e;
}

bool source_boundary_untouched(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    for (size_t node_id : decision.carrier_nodes) {
        if (std::ranges::find(plan.disable_nodes, node_id) != plan.disable_nodes.end()) return false;
    }
    for (size_t edge_index : decision.carrier_exit_edges) {
        if (std::ranges::find(plan.disable_edges, edge_index) != plan.disable_edges.end()) return false;
        if (std::ranges::any_of(plan.redirect_edges, [&](const auto & redirect) {
                if (redirect.edge_index != edge_index) return false;
                if (redirect.effect_id == decision.effect_id) return true;
                return redirect.src.has_value();
            }))
            return false;
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
    const auto ingress = target_effect_ingress(decision);
    if ((!ingress.existing_node_id && ingress.synthetic_id.empty())
        || !has_added_edge(plan, ingress, core::DagNodeRef::synthetic(decision.synthetic_id), decision.effect_id))
        return false;
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

bool consumer_dependencies(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (decision.target_host_control_terminal) {
        return has_added_edge(plan,
                              core::DagNodeRef::synthetic(decision.synthetic_id),
                              core::DagNodeRef::synthetic(decision.target_host_control_terminal_join_synthetic_id),
                              decision.effect_id);
    }
    if (decision.consumer_anchors.empty()) {
        if (!decision.request_consumer_synthetic_id.empty())
            return has_added_edge(plan,
                                  core::DagNodeRef::synthetic(decision.synthetic_id),
                                  core::DagNodeRef::synthetic(decision.request_consumer_synthetic_id),
                                  "hicache_request_io_dependency");
        return !decision.family_consumer_synthetic_id.empty()
                   && has_added_edge(plan,
                                     core::DagNodeRef::synthetic(decision.synthetic_id),
                                     core::DagNodeRef::synthetic(decision.family_consumer_synthetic_id),
                                     "hicache_family_dependency")
               || decision.effect_type == model::HiCacheEffectType::CommitDeviceToHost || decision.effect_type == model::HiCacheEffectType::CommitHostToStorage;
    }
    return std::ranges::all_of(decision.consumer_anchors, [&](size_t consumer) {
        return has_added_edge(plan, core::DagNodeRef::synthetic(decision.synthetic_id), core::DagNodeRef::existing(consumer), decision.effect_id);
    });
}

bool reused_readiness_consumer_dependencies(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.consumer_anchors.empty()) return true;
    if (decision.family_consumer_synthetic_id.empty()) return false;
    const auto & endpoints = decision.source_completion_node_ids.empty() ? decision.owned_duration_nodes : decision.source_completion_node_ids;
    return !endpoints.empty() && std::ranges::all_of(endpoints, [&](size_t endpoint) {
        return has_added_edge(plan,
                              core::DagNodeRef::existing(endpoint),
                              core::DagNodeRef::synthetic(decision.family_consumer_synthetic_id),
                              "hicache_family_dependency");
    });
}

bool insertion_dependencies(const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    const auto ingress = target_effect_ingress(decision);
    return (ingress.existing_node_id || !ingress.synthetic_id.empty())
           && has_added_edge(plan, ingress, core::DagNodeRef::synthetic(decision.synthetic_id), decision.effect_id);
}

bool completion_join_dependencies(const core::DagGraph & graph, const core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.completion_join_required || !decision.control_ready_anchor_node_id || !decision.wait_exit_anchor_node_id) return false;
    const auto ingress = target_effect_ingress(decision);
    if ((!ingress.existing_node_id && ingress.synthetic_id.empty())
        || !has_added_edge(plan, ingress, core::DagNodeRef::synthetic(decision.synthetic_id), decision.effect_id)
        || !has_added_edge(plan,
                           core::DagNodeRef::synthetic(decision.synthetic_id),
                           core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
                           decision.effect_id)
        || (decision.target_host_control_terminal ? (!has_added_edge(plan,
                                                                     core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
                                                                     core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
                                                                     decision.effect_id)
                                                     || !has_added_edge(plan,
                                                                        core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
                                                                        core::DagNodeRef::existing(*decision.wait_exit_anchor_node_id),
                                                                        decision.effect_id))
                                                  : !has_added_edge(plan,
                                                                    core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
                                                                    core::DagNodeRef::existing(*decision.wait_exit_anchor_node_id),
                                                                    decision.effect_id)))
        return false;
    const bool control_ready = [&] {
        if (*decision.control_ready_anchor_node_id != *decision.wait_exit_anchor_node_id) {
            return has_added_edge(plan,
                                  core::DagNodeRef::existing(*decision.control_ready_anchor_node_id),
                                  core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
                                  decision.effect_id);
        }
        if (!decision.completion_control_ingress_edge_id || *decision.completion_control_ingress_edge_id >= graph.edge_count()) return false;
        const auto * redirect = redirected_ingress(plan, *decision.completion_control_ingress_edge_id, decision);
        return redirect != nullptr && redirect->dst && ref_is_synthetic(*redirect->dst, decision.completion_join_synthetic_id);
    }();
    return control_ready;
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
        validation.observable_endpoint_ready = true;
        validation.ingress_preserved = true;
        validation.egress_preserved = true;
        validation.consumer_dependency_ready = true;
        validation.reason = "source and target both omit the effect";
        return validation;
    }

    validation.observable_endpoint_ready = observable_endpoint_contract(shadow.plan, decision);

    if (decision.source_readiness_topology_reused) {
        const bool duration_ready = readiness_topology_duration_replaced(shadow.plan, decision);
        const bool control_ready = source_control_removed(graph, shadow.plan, decision);
        const bool target_control_ready = target_host_control_materialized(shadow.plan, decision);
        const bool topology_ready =
            decision.completion_join_contract_ready && !decision.owned_duration_nodes.empty() && source_boundary_untouched(shadow.plan, decision);
        validation.source_cost_removed = duration_ready && control_ready;
        validation.target_cost_materialized = duration_ready && target_control_ready;
        validation.ingress_preserved = topology_ready;
        validation.egress_preserved = topology_ready;
        validation.consumer_dependency_ready = topology_ready && reused_readiness_consumer_dependencies(shadow.plan, decision);
        validation.ready = duration_ready && control_ready && target_control_ready && topology_ready && validation.consumer_dependency_ready
                           && validation.observable_endpoint_ready;
        validation.reason = validation.ready
                                ? "target transfer duration and source host-control removal are materialized on the source event record/wait topology"
                                : "source readiness-topology reuse failed duration, host-control, or boundary validation";
        return validation;
    }

    const bool insertion = decision.rewrite_kind == HiCacheRewriteKind::InsertIo || decision.rewrite_kind == HiCacheRewriteKind::InsertGate;
    const bool removal = decision.rewrite_kind == HiCacheRewriteKind::RemoveOwnedCost || decision.rewrite_kind == HiCacheRewriteKind::RemoveDependency;
    const bool gate = decision.rewrite_kind == HiCacheRewriteKind::ReplaceWithGate || decision.rewrite_kind == HiCacheRewriteKind::InsertGate;
    const bool io_replacement = decision.rewrite_kind == HiCacheRewriteKind::ReplaceWithIo || decision.rewrite_kind == HiCacheRewriteKind::PartialReplace;
    validation.source_cost_removed = insertion || gate || duration_removed(graph, shadow.plan, decision);
    validation.egress_preserved = insertion || decision.target_host_control_terminal || source_boundary_untouched(shadow.plan, decision);
    if (removal) {
        validation.target_cost_materialized = true;
        validation.ingress_preserved = source_boundary_untouched(shadow.plan, decision);
        validation.consumer_dependency_ready = validation.egress_preserved;
    }
    else {
        const auto * synthetic = synthetic_node(shadow.plan, decision);
        if (decision.completion_join_required) {
            const auto * join = completion_join_node(shadow.plan, decision);
            const bool dependencies = completion_join_dependencies(graph, shadow.plan, decision);
            validation.target_cost_materialized = synthetic != nullptr && synthetic->node.duration == decision.duration_us && join != nullptr
                                                  && join->node.duration == 0 && target_host_control_materialized(shadow.plan, decision);
            validation.ingress_preserved = dependencies;
            validation.consumer_dependency_ready = dependencies;
        }
        else {
            validation.target_cost_materialized =
                synthetic != nullptr && synthetic->node.duration == decision.duration_us && target_host_control_materialized(shadow.plan, decision);
            validation.ingress_preserved =
                (insertion || gate || io_replacement) ? insertion_dependencies(shadow.plan, decision) : replacement_ingress(graph, shadow.plan, decision);
            validation.consumer_dependency_ready = synthetic != nullptr && consumer_dependencies(shadow.plan, decision);
        }
    }
    validation.ready = validation.source_cost_removed && validation.target_cost_materialized && validation.observable_endpoint_ready
                       && validation.ingress_preserved && validation.egress_preserved && validation.consumer_dependency_ready;
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
