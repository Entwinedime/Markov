#include "rewrite_mutation.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

using model::HiCacheEffectType;
using model::HiCacheSourceCarrierState;

void append_source_control_updates(const core::DagGraph & graph, core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::NoOp || decision.rewrite_kind == HiCacheRewriteKind::Reject
        || !decision.source_control_removal_required)
        return;
    for (size_t node_id : decision.source_control_duration_nodes) {
        plan.set_node_durations.push_back(core::DagSetNodeDurationMutation{
            .node_id = node_id,
            .duration = 0,
            .effect_id = decision.effect_id,
            .reason = "remove exact snapshot-exclusive source load-back host-control before target control materialization",
        });
    }
    (void)graph;
}

void append_target_host_control_node(core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.target_host_control_required) return;
    if (!decision.target_host_control_terminal && !decision.target_host_control_anchor_node_id)
        throw std::logic_error("HiCache target load-back host-control requires a proven executable CPU anchor");
    plan.synthetic_nodes.push_back(core::DagSyntheticNodeMutation{
        .synthetic_id = decision.target_host_control_synthetic_id,
        .node = core::DagSyntheticNodeSpec{
            .name = "hicache_" + model::hicache_effect_type_name(decision.effect_type) + "_host_control",
            .category = "hicache_patch",
            .is_cpu = true,
            .lane_key = decision.cache_scope.empty() ? "hicache_host_control_lane" : decision.cache_scope + "/host_control_lane",
            .duration = decision.target_host_control_duration_us,
            .counts_toward_e2e = false,
            .attrs = {
                { "effect_id", decision.effect_id },
                { "cost_model", decision.target_host_control_terminal ? "zero_payload_terminal_control" : "fixed_plus_per_target_page" },
            },
        },
        .effect_id = decision.effect_id,
        .reason = decision.target_host_control_terminal
                      ? "materialize target zero-payload HiCache terminal progress control"
                      : "materialize target HiCache CPU submission from the workflow-wide host-control calibration",
    });
    if (decision.target_host_control_terminal) {
        if (decision.completion_join_required || decision.target_host_control_ingress_edge_ids.empty()
            || decision.target_host_control_exit_node_ids.size() != 1)
            throw std::logic_error("HiCache zero-payload terminal control requires consumer readiness ingresses and one exit");
        plan.synthetic_nodes.push_back(core::DagSyntheticNodeMutation{
            .synthetic_id = decision.target_host_control_terminal_join_synthetic_id,
            .node = core::DagSyntheticNodeSpec{
                .name = "hicache_zero_payload_terminal_control_join",
                .category = "hicache_patch",
                .is_cpu = false,
                .lane_key = "hicache_terminal_control_join",
                .duration = 0,
                .counts_toward_e2e = false,
                .attrs = {
                    { "effect_id", decision.effect_id },
                    { "join_semantics", "max_consumer_readiness_before_terminal_control" },
                },
            },
            .effect_id = decision.effect_id,
            .reason = "derive the terminal-control opportunity from all real consumer prerequisites",
        });
        for (size_t ingress_edge_id : decision.target_host_control_ingress_edge_ids) {
            plan.redirect_edges.push_back(core::DagRedirectEdgeMutation{
                .edge_index = ingress_edge_id,
                .dst = core::DagNodeRef::synthetic(decision.target_host_control_terminal_join_synthetic_id),
                .effect_id = decision.effect_id,
                .reason = "collect one real consumer prerequisite before target zero-payload terminal control",
            });
        }
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.target_host_control_terminal_join_synthetic_id),
            .dst = core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "zero-payload terminal control begins after every real consumer prerequisite is ready",
        });
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
            .dst = core::DagNodeRef::existing(decision.target_host_control_exit_node_ids.front()),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "target consumer begins after zero-payload terminal progress control completes",
        });
        return;
    }
    plan.add_edges.push_back(core::DagAddEdgeMutation{
        .src = core::DagNodeRef::existing(*decision.target_host_control_anchor_node_id),
        .dst = core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
        .kind = core::DagEdgeKind::Mutation,
        .effect_id = decision.effect_id,
        .reason = "target HiCache host-control begins at the proven source opportunity boundary",
    });
    for (size_t exit_node_id : decision.target_host_control_exit_node_ids) {
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
            .dst = core::DagNodeRef::existing(exit_node_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "every logical-input CPU lane waits for target load-back host submission to complete",
        });
    }
}

core::DagNodeRef target_effect_ingress(const HiCacheRewriteDecision & decision) {
    if (decision.target_host_control_required && !decision.target_host_control_terminal)
        return core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id);
    if (!decision.source_execution_anchor_node_id) throw std::logic_error("HiCache target effect requires a proven executable source anchor");
    return core::DagNodeRef::existing(*decision.source_execution_anchor_node_id);
}

void append_reused_readiness_host_control_edges(core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.target_host_control_required) return;
    if (decision.owned_duration_nodes.empty()) throw std::logic_error("HiCache target host-control cannot precede an empty carried readiness topology");
    for (size_t node_id : decision.owned_duration_nodes) {
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
            .dst = core::DagNodeRef::existing(node_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "target H2D transfer cannot begin before target load-back host submission completes",
        });
    }
}

void append_duration_updates(const core::DagGraph & graph, core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    for (size_t node_id : decision.owned_duration_nodes) {
        plan.set_node_durations.push_back(core::DagSetNodeDurationMutation{
            .node_id = node_id,
            .duration = 0,
            .effect_id = decision.effect_id,
            .reason = "remove source-owned HiCache duration while retaining boundary identity and CPU gap",
        });
    }
    (void)graph;
}

void append_readiness_topology_duration_updates(const core::DagGraph & graph, core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (decision.owned_duration_nodes.empty()) throw std::logic_error("HiCache readiness-topology reuse requires source device-transfer nodes");
    std::vector<uint64_t> weights;
    weights.reserve(decision.owned_duration_nodes.size());
    uint64_t total_weight = 0;
    for (size_t node_id : decision.owned_duration_nodes) {
        if (node_id >= graph.node_count() || !graph.node(node_id).active || graph.node(node_id).is_cpu)
            throw std::logic_error("HiCache readiness-topology reuse owns an invalid device-transfer node");
        auto weight = graph.event_for_node(node_id).arg_u64("size(B)", 0);
        if (weight == 0) weight = graph.node(node_id).original_duration;
        if (weight == 0) weight = 1;
        weights.push_back(weight);
        total_weight = core::checked_add_u64(total_weight, weight, "HiCache readiness-topology weight exceeds uint64 range");
    }
    uint64_t assigned = 0;
    for (size_t index = 0; index < decision.owned_duration_nodes.size(); ++index) {
        uint64_t duration = 0;
        if (index + 1 == decision.owned_duration_nodes.size()) duration = decision.duration_us - assigned;
        else {
            const auto projected = core::floor_multiply_divide_u64(decision.duration_us, weights[index], total_weight);
            if (!projected) throw std::overflow_error("HiCache readiness-topology duration projection overflow");
            duration = *projected;
            assigned = core::checked_add_u64(assigned, duration, "HiCache readiness-topology assigned duration exceeds target duration");
        }
        plan.set_node_durations.push_back(core::DagSetNodeDurationMutation{
            .node_id = decision.owned_duration_nodes[index],
            .duration = duration,
            .effect_id = decision.effect_id,
            .reason = "replace the source device-transfer duration while preserving its event record/wait readiness topology",
        });
    }
}

void append_gap_updates(const core::DagGraph & graph, core::DagMutationPlan & plan, const std::vector<HiCacheRewriteDecision> & decisions) {
    std::map<size_t, std::vector<std::pair<uint64_t, uint64_t>>> slices_by_node;
    for (const auto & decision : decisions) {
        if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::NoOp || decision.rewrite_kind == HiCacheRewriteKind::Reject) continue;
        const auto append = [&](const HiCacheCpuGapSlice & gap) {
            if (gap.owned_end_us > gap.owned_start_us) slices_by_node[gap.owner_node_id].emplace_back(gap.owned_start_us, gap.owned_end_us);
        };
        if (decision.completion_join_required) {
            for (const auto & gap : decision.completion_wait_slices) append(gap);
            for (const auto & gap : decision.logical_input_completion_wait_slices) append(gap);
        }
        else {
            for (const auto & gap : decision.owned_gap_slices) append(gap);
            for (const auto & gap : decision.logical_input_causal_gap_slices) append(gap);
        }
        if (decision.source_control_removal_required) {
            for (const auto & gap : decision.source_control_gap_slices) append(gap);
        }
    }
    for (auto & [node_id, slices] : slices_by_node) {
        if (node_id >= graph.node_count()) continue;
        std::ranges::sort(slices);
        uint64_t owned = 0;
        std::pair<uint64_t, uint64_t> current{};
        bool has_current = false;
        for (const auto & slice : slices) {
            if (!has_current) {
                current = slice;
                has_current = true;
            }
            else if (slice.first <= current.second) current.second = std::max(current.second, slice.second);
            else {
                owned = core::checked_add_u64(owned, current.second - current.first, "HiCache projected CPU gap exceeds uint64 range");
                current = slice;
            }
        }
        if (has_current) owned = core::checked_add_u64(owned, current.second - current.first, "HiCache projected CPU gap exceeds uint64 range");
        if (owned == 0 || owned > graph.node(node_id).cpu_gap_after) continue;
        plan.set_cpu_gaps.push_back(core::DagSetCpuGapMutation{
            .node_id = node_id,
            .duration = graph.node(node_id).cpu_gap_after - owned,
            .effect_id = "hicache_foreground_wait_projection",
            .reason = "remove the union of source-owned foreground HiCache wait from every idle CPU lane in the same logical input",
        });
    }
}

void append_completion_join_node(core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    plan.synthetic_nodes.push_back(core::DagSyntheticNodeMutation{
        .synthetic_id = decision.completion_join_synthetic_id,
        .node = core::DagSyntheticNodeSpec{
            .name = "hicache_io_completion_join",
            .category = "hicache_patch",
            .is_cpu = false,
            .lane_key = "hicache_completion_join",
            .duration = 0,
            .counts_toward_e2e = false,
            .attrs = {
                { "effect_id", decision.effect_id },
                { "join_semantics", "max_control_ready_io_complete" },
            },
        },
        .effect_id = decision.effect_id,
        .reason = "zero-duration max-plus join between control readiness and target I/O completion",
    });
}

void append_completion_join_edges(const core::DagGraph & graph, core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.source_execution_anchor_node_id || !decision.control_ready_anchor_node_id || !decision.wait_exit_anchor_node_id)
        throw std::logic_error("HiCache completion join requires source, control-ready, and wait-exit anchors");
    plan.add_edges.push_back(core::DagAddEdgeMutation{
        .src = target_effect_ingress(decision),
        .dst = core::DagNodeRef::synthetic(decision.synthetic_id),
        .kind = core::DagEdgeKind::Mutation,
        .effect_id = decision.effect_id,
        .reason = "target prefetch becomes eligible after its source opportunity anchor",
    });
    plan.add_edges.push_back(core::DagAddEdgeMutation{
        .src = core::DagNodeRef::synthetic(decision.synthetic_id),
        .dst = core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
        .kind = core::DagEdgeKind::Mutation,
        .effect_id = decision.effect_id,
        .reason = "target storage completion supplies the I/O branch of the completion join",
    });
    if (*decision.control_ready_anchor_node_id == *decision.wait_exit_anchor_node_id) {
        if (!decision.completion_control_ingress_edge_id)
            throw std::logic_error("Immediate-ready HiCache completion join requires one sequential control ingress");
        plan.redirect_edges.push_back(core::DagRedirectEdgeMutation{
            .edge_index = *decision.completion_control_ingress_edge_id,
            .dst = core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
            .effect_id = decision.effect_id,
            .reason = "preserve the original CPU gap as the control-ready branch of an immediate-ready completion join",
        });
    }
    else {
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::existing(*decision.control_ready_anchor_node_id),
            .dst = core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "counterfactual foreground control readiness supplies the control branch of the completion join",
        });
    }
    if (decision.target_host_control_required && decision.target_host_control_terminal) {
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
            .dst = core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "zero-payload terminal progress control begins after both completion-join branches are ready",
        });
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.target_host_control_synthetic_id),
            .dst = core::DagNodeRef::existing(*decision.wait_exit_anchor_node_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "completion wait exits after target zero-payload terminal control completes",
        });
    }
    else {
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.completion_join_synthetic_id),
            .dst = core::DagNodeRef::existing(*decision.wait_exit_anchor_node_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "one retained terminal progress check begins after both completion-join branches are ready",
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
            .counts_toward_e2e = false,
            .attrs = {
                { "effect_id", decision.effect_id },
                { "rewrite_kind", hicache_rewrite_kind_name(decision.rewrite_kind) },
            },
        },
        .effect_id = decision.effect_id,
        .reason = decision.reason,
    });
}

void append_insertion_edges(core::DagMutationPlan & plan, const HiCacheRewriteDecision & decision) {
    if (!decision.source_execution_anchor_node_id) throw std::logic_error("HiCache insertion requires a proven executable source anchor");
    plan.add_edges.push_back(core::DagAddEdgeMutation{
        .src = target_effect_ingress(decision),
        .dst = core::DagNodeRef::synthetic(decision.synthetic_id),
        .kind = core::DagEdgeKind::Mutation,
        .effect_id = decision.effect_id,
        .reason = "target effect becomes eligible after its source opportunity anchor",
    });
    if (decision.target_host_control_terminal) {
        plan.add_edges.push_back(core::DagAddEdgeMutation{
            .src = core::DagNodeRef::synthetic(decision.synthetic_id),
            .dst = core::DagNodeRef::synthetic(decision.target_host_control_terminal_join_synthetic_id),
            .kind = core::DagEdgeKind::Mutation,
            .effect_id = decision.effect_id,
            .reason = "zero-payload target I/O boundary joins the real consumer prerequisites before terminal control",
        });
        return;
    }
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

void append_e2e_eligibility_updates(core::DagMutationPlan & plan, const std::vector<HiCacheRewriteDecision> & decisions) {
    std::set<size_t> infrastructure_nodes;
    for (const auto & decision : decisions) {
        if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::NoOp || decision.rewrite_kind == HiCacheRewriteKind::Reject) continue;
        infrastructure_nodes.insert(decision.owned_duration_nodes.begin(), decision.owned_duration_nodes.end());
        infrastructure_nodes.insert(decision.source_completion_node_ids.begin(), decision.source_completion_node_ids.end());
        infrastructure_nodes.insert(decision.readiness_join_node_ids.begin(), decision.readiness_join_node_ids.end());
        infrastructure_nodes.insert(decision.completion_wait_owned_node_ids.begin(), decision.completion_wait_owned_node_ids.end());
    }
    for (size_t node_id : infrastructure_nodes) {
        plan.set_node_e2e_eligibility.push_back(core::DagSetNodeE2eEligibilityMutation{
            .node_id = node_id,
            .counts_toward_e2e = false,
            .effect_id = "hicache_observable_endpoint_contract",
            .reason = "model-owned I/O infrastructure affects business E2E only through dependencies to observable nodes",
        });
    }
}

core::DagMutationPlan build_plan(const core::DagGraph & graph, const std::vector<HiCacheRewriteDecision> & decisions, const HiCacheIoResourcePlan & resources) {
    core::DagMutationPlan plan{
        .component = "hicache_direct",
        .reason = "read-only prospective HiCache rewrite transaction",
    };
    std::unordered_map<std::string, std::string> synthetic_by_effect;
    for (const auto & decision : decisions) {
        if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::NoOp || decision.rewrite_kind == HiCacheRewriteKind::Reject) continue;
        append_target_host_control_node(plan, decision);
        if (decision.rewrite_kind == HiCacheRewriteKind::RemoveOwnedCost || decision.rewrite_kind == HiCacheRewriteKind::RemoveDependency) {
            if (decision.source_readiness_topology_reused) append_readiness_topology_duration_updates(graph, plan, decision);
            else append_duration_updates(graph, plan, decision);
            continue;
        }
        if (decision.source_readiness_topology_reused) {
            append_readiness_topology_duration_updates(graph, plan, decision);
            append_reused_readiness_host_control_edges(plan, decision);
            continue;
        }
        append_synthetic_node(plan, decision);
        synthetic_by_effect.emplace(decision.effect_id, decision.synthetic_id);
        if (decision.completion_join_required) {
            append_completion_join_node(plan, decision);
            append_duration_updates(graph, plan, decision);
            append_completion_join_edges(graph, plan, decision);
            continue;
        }
        if (decision.rewrite_kind == HiCacheRewriteKind::ReplaceWithGate || decision.rewrite_kind == HiCacheRewriteKind::InsertGate) {
            append_gate_edges(plan, decision);
            continue;
        }
        if (decision.source_carrier_state == HiCacheSourceCarrierState::Present) {
            append_duration_updates(graph, plan, decision);
            append_insertion_edges(plan, decision);
        }
        else append_insertion_edges(plan, decision);
    }
    for (const auto & decision : decisions) append_source_control_updates(graph, plan, decision);
    append_e2e_eligibility_updates(plan, decisions);
    append_gap_updates(graph, plan, decisions);
    append_resource_lane_dependencies(plan, resources, synthetic_by_effect);
    append_family_dependencies(plan, decisions, synthetic_by_effect);
    append_reused_readiness_family_dependencies(plan, decisions);
    append_request_io_dependencies(plan, decisions, synthetic_by_effect);
    return plan;
}


} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
