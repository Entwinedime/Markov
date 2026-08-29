/**
 * @file
 * @brief Post-apply semantic validation for HiCache DAG rewrites.
 */
#include "markov/trace_graph/modules/hicache/patch/applied_validator.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>

namespace markov::trace_graph::modules::hicache::patch {

namespace applied_validator_detail {

using model::HiCacheEffectType;

using EdgeKey = std::tuple<size_t, size_t, core::DagEdgeKind, std::string>;

void add_blocker(HiCacheAppliedPatchValidation & validation, std::string blocker) {
    (void)core::checked_increment_u64(validation.blocker_counts[std::move(blocker)], "HiCache applied-patch blocker count exceeds uint64 range");
}

bool synthetic_rewrite(HiCacheRewriteKind kind) {
    return kind == HiCacheRewriteKind::ReplaceWithIo || kind == HiCacheRewriteKind::ReplaceWithGate || kind == HiCacheRewriteKind::InsertIo
           || kind == HiCacheRewriteKind::InsertGate || kind == HiCacheRewriteKind::PartialReplace;
}

bool insertion_rewrite(HiCacheRewriteKind kind) {
    return kind == HiCacheRewriteKind::ReplaceWithGate || kind == HiCacheRewriteKind::InsertIo || kind == HiCacheRewriteKind::InsertGate;
}

bool replacement_io(HiCacheRewriteKind kind) { return kind == HiCacheRewriteKind::ReplaceWithIo || kind == HiCacheRewriteKind::PartialReplace; }

std::optional<size_t> resolve_ref(const core::DagNodeRef & ref, const std::unordered_map<std::string, size_t> & synthetic_nodes) {
    if (ref.existing_node_id) return ref.existing_node_id;
    const auto found = synthetic_nodes.find(ref.synthetic_id);
    return found == synthetic_nodes.end() ? std::nullopt : std::optional<size_t>{ found->second };
}

bool edge_matches(const core::DagGraph & graph, size_t edge_index, const EdgeKey & expected) {
    if (edge_index >= graph.edge_count()) return false;
    const auto & edge = graph.edge(edge_index);
    const auto & [src, dst, kind, effect_id] = expected;
    return edge.active && edge.src == src && edge.dst == dst && edge.kind == kind && edge.effect_id() == effect_id;
}

struct MaterializedPlan {
    bool ready = true;
    std::unordered_map<std::string, size_t> synthetic_nodes;
    std::set<EdgeKey> added_edges;
    std::set<size_t> redirected_edges;
    std::set<std::pair<size_t, std::string>> duration_updates;
    std::set<size_t> e2e_eligibility_updates;
    std::set<std::pair<size_t, std::string>> cpu_gap_updates;
};

const core::DagMutationRecord * unique_record(const core::DagMutationJournal & journal, core::DagMutationAction action, const auto & predicate) {
    const core::DagMutationRecord * match = nullptr;
    for (const auto & record : journal.records) {
        if (record.action != action || !predicate(record)) continue;
        if (match != nullptr) return nullptr;
        match = &record;
    }
    return match;
}

MaterializedPlan materialize_plan_index(const core::DagGraph & graph, const core::DagMutationPlan & plan, const core::DagMutationJournal & journal,
                                        HiCacheAppliedPatchValidation & validation) {
    MaterializedPlan output;
    for (const auto & synthetic : plan.synthetic_nodes) {
        const auto * record = unique_record(journal, core::DagMutationAction::AddSyntheticNode, [&](const auto & candidate) {
            if (candidate.effect_id != synthetic.effect_id || !candidate.node_id || *candidate.node_id >= graph.node_count()) return false;
            return graph.event_for_node(*candidate.node_id).arg("synthetic_id") == synthetic.synthetic_id;
        });
        if (record == nullptr || !record->node_id || *record->node_id >= graph.node_count()) {
            output.ready = false;
            add_blocker(validation, "synthetic_node_journal_mismatch");
            continue;
        }
        const auto node_id = *record->node_id;
        const auto & node = graph.node(node_id);
        const auto & event = graph.event_for_node(node_id);
        const bool exact = node.active && node.kind == core::DagNodeKind::Synthetic && event.arg("synthetic_id") == synthetic.synthetic_id
                           && event.arg("effect_id") == synthetic.effect_id;
        if (!exact || !output.synthetic_nodes.emplace(synthetic.synthetic_id, node_id).second) {
            output.ready = false;
            add_blocker(validation, "synthetic_node_materialization_mismatch");
        }
    }

    for (const auto & update : plan.set_node_durations) {
        const auto record_count = std::ranges::count_if(journal.records, [&](const auto & candidate) {
            return candidate.action == core::DagMutationAction::SetNodeDuration && candidate.node_id == update.node_id
                   && candidate.effect_id == update.effect_id && candidate.new_duration == update.duration;
        });
        if (record_count > 1 || update.node_id >= graph.node_count() || graph.node(update.node_id).duration != update.duration) {
            output.ready = false;
            add_blocker(validation, "source_duration_materialization_mismatch");
            continue;
        }
        output.duration_updates.emplace(update.node_id, update.effect_id);
    }

    for (const auto & update : plan.set_node_e2e_eligibility) {
        const auto record_count = std::ranges::count_if(journal.records, [&](const auto & candidate) {
            return candidate.action == core::DagMutationAction::SetNodeE2eEligibility && candidate.node_id == update.node_id
                   && candidate.effect_id == update.effect_id && candidate.new_counts_toward_e2e == update.counts_toward_e2e;
        });
        if (record_count > 1 || update.node_id >= graph.node_count() || graph.node(update.node_id).counts_toward_e2e != update.counts_toward_e2e) {
            output.ready = false;
            add_blocker(validation, "source_e2e_eligibility_materialization_mismatch");
            continue;
        }
        output.e2e_eligibility_updates.insert(update.node_id);
    }

    for (const auto & update : plan.set_cpu_gaps) {
        const auto record_count = std::ranges::count_if(journal.records, [&](const auto & candidate) {
            return candidate.action == core::DagMutationAction::SetCpuGap && candidate.node_id == update.node_id && candidate.effect_id == update.effect_id
                   && candidate.new_cpu_gap == update.duration;
        });
        if (record_count > 1 || update.node_id >= graph.node_count() || graph.node(update.node_id).cpu_gap_after != update.duration) {
            output.ready = false;
            add_blocker(validation, "source_cpu_gap_materialization_mismatch");
            continue;
        }
        output.cpu_gap_updates.emplace(update.node_id, update.effect_id);
    }

    for (const auto & redirect : plan.redirect_edges) {
        const auto * record = unique_record(journal, core::DagMutationAction::RedirectEdge, [&](const auto & candidate) {
            return candidate.replaced_edge_index == redirect.edge_index && candidate.effect_id == redirect.effect_id;
        });
        const auto src = record == nullptr ? std::nullopt : record->src;
        const auto dst = record == nullptr ? std::nullopt : record->dst;
        if (record == nullptr || !src || !dst || redirect.edge_index >= graph.edge_count() || graph.edge(redirect.edge_index).active) {
            output.ready = false;
            add_blocker(validation, "redirect_materialization_mismatch");
            continue;
        }
        const auto & original = graph.edge(redirect.edge_index);
        const EdgeKey expected{ *src, *dst, original.kind, redirect.effect_id };
        const bool replacement_ready =
            record->edge_index ? edge_matches(graph, *record->edge_index, expected) : graph.has_active_edge(*src, *dst, original.kind, redirect.effect_id);
        if (!replacement_ready) {
            output.ready = false;
            add_blocker(validation, "redirect_replacement_edge_missing");
            continue;
        }
        output.redirected_edges.insert(redirect.edge_index);
    }

    for (const auto & addition : plan.add_edges) {
        const auto src = resolve_ref(addition.src, output.synthetic_nodes);
        const auto dst = resolve_ref(addition.dst, output.synthetic_nodes);
        if (!src || !dst) {
            output.ready = false;
            add_blocker(validation, "added_edge_endpoint_unresolved");
            continue;
        }
        const EdgeKey expected{ *src, *dst, addition.kind, addition.effect_id };
        const auto * record = unique_record(journal, core::DagMutationAction::AddEdge, [&](const auto & candidate) {
            return candidate.src == src && candidate.dst == dst && candidate.effect_id == addition.effect_id;
        });
        if (record == nullptr || !record->edge_index || !edge_matches(graph, *record->edge_index, expected)) {
            output.ready = false;
            add_blocker(validation, "added_edge_materialization_mismatch");
            continue;
        }
        output.added_edges.insert(expected);
    }
    return output;
}

bool source_control_exact(const HiCacheRewriteDecision & decision, const core::DagGraph & graph, const MaterializedPlan & materialized) {
    if (!decision.source_control_removal_required) return true;
    if (decision.source_control_duration_nodes.empty() && decision.source_control_gap_slices.empty()) return false;
    const bool nodes_exact = std::ranges::all_of(decision.source_control_duration_nodes, [&](size_t node_id) {
        return node_id < graph.node_count() && graph.node(node_id).duration == 0 && materialized.duration_updates.contains({ node_id, decision.effect_id });
    });
    const bool gaps_exact = std::ranges::all_of(decision.source_control_gap_slices, [&](const auto & gap) {
        return gap.owner_node_id < graph.node_count()
               && std::ranges::any_of(materialized.cpu_gap_updates, [&](const auto & update) { return update.first == gap.owner_node_id; });
    });
    return nodes_exact && gaps_exact;
}

bool duration_exact(const HiCacheRewriteDecision & decision, const core::DagGraph & graph, const MaterializedPlan & materialized) {
    if (decision.source_readiness_topology_reused) {
        uint64_t total = 0;
        for (size_t node_id : decision.owned_duration_nodes) {
            if (node_id >= graph.node_count() || !materialized.duration_updates.contains({ node_id, decision.effect_id })) return false;
            total = core::checked_add_u64(total, graph.node(node_id).duration, "HiCache applied readiness duration exceeds uint64 range");
        }
        return !decision.owned_duration_nodes.empty() && total == decision.duration_us && source_control_exact(decision, graph, materialized);
    }
    const bool nodes_exact = std::ranges::all_of(decision.owned_duration_nodes, [&](size_t node_id) {
        return node_id < graph.node_count() && graph.node(node_id).duration == 0 && materialized.duration_updates.contains({ node_id, decision.effect_id });
    });
    const auto gap_exact = [&](const auto & gap) {
        return gap.owner_node_id < graph.node_count()
               && std::ranges::any_of(materialized.cpu_gap_updates, [&](const auto & update) { return update.first == gap.owner_node_id; });
    };
    const auto & owned_gaps = decision.completion_join_required ? decision.completion_wait_slices : decision.owned_gap_slices;
    const auto & causal_gaps = decision.completion_join_required ? decision.logical_input_completion_wait_slices : decision.logical_input_causal_gap_slices;
    const bool gaps_exact = std::ranges::all_of(owned_gaps, gap_exact);
    const bool causal_gaps_exact = std::ranges::all_of(causal_gaps, gap_exact);
    return nodes_exact && gaps_exact && causal_gaps_exact && source_control_exact(decision, graph, materialized);
}

bool added_edge_exists(const MaterializedPlan & materialized, size_t src, size_t dst, std::string_view effect_id) {
    return materialized.added_edges.contains(EdgeKey{ src, dst, core::DagEdgeKind::Mutation, std::string(effect_id) });
}

bool target_host_control_exact(const HiCacheRewriteDecision & decision, const core::DagGraph & graph, const MaterializedPlan & materialized) {
    const auto found = materialized.synthetic_nodes.find(decision.target_host_control_synthetic_id);
    if (!decision.target_host_control_required) return found == materialized.synthetic_nodes.end();
    if (found == materialized.synthetic_nodes.end()
        || (decision.target_host_control_exit_node_ids.empty() && decision.effect_type != HiCacheEffectType::CommitDeviceToHost))
        return false;
    const auto node_id = found->second;
    const auto expected_lane = decision.cache_scope.empty() ? std::string{ "hicache_host_control_lane" } : decision.cache_scope + "/host_control_lane";
    if (!graph.node(node_id).active || !graph.node(node_id).is_cpu || graph.node(node_id).counts_toward_e2e
        || graph.node(node_id).duration != decision.target_host_control_duration_us || graph.node_lane_key(node_id) != expected_lane
        || !std::ranges::all_of(decision.target_host_control_exit_node_ids,
                                [&](size_t exit_node_id) { return added_edge_exists(materialized, node_id, exit_node_id, decision.effect_id); }))
        return false;
    if (decision.target_host_control_terminal) {
        if (decision.completion_join_required || decision.target_host_control_exit_node_ids.size() != 1
            || decision.target_host_control_ingress_edge_ids.empty())
            return false;
        const auto join = materialized.synthetic_nodes.find(decision.target_host_control_terminal_join_synthetic_id);
        return join != materialized.synthetic_nodes.end() && graph.node(join->second).active && !graph.node(join->second).counts_toward_e2e
               && graph.node(join->second).duration == 0 && graph.node_lane_key(join->second) == "hicache_terminal_control_join"
               && added_edge_exists(materialized, join->second, node_id, decision.effect_id)
               && std::ranges::all_of(decision.target_host_control_ingress_edge_ids,
                                      [&](size_t edge_id) { return materialized.redirected_edges.contains(edge_id); });
    }
    if (!decision.target_host_control_anchor_node_id
        || !added_edge_exists(materialized, *decision.target_host_control_anchor_node_id, node_id, decision.effect_id))
        return false;
    if (!decision.source_readiness_topology_reused) {
        const auto effect = materialized.synthetic_nodes.find(decision.synthetic_id);
        return effect != materialized.synthetic_nodes.end() && added_edge_exists(materialized, node_id, effect->second, decision.effect_id);
    }
    return !decision.owned_duration_nodes.empty() && std::ranges::all_of(decision.owned_duration_nodes, [&](size_t transfer_node_id) {
        return added_edge_exists(materialized, node_id, transfer_node_id, decision.effect_id);
    });
}

bool synthetic_exact(const HiCacheRewriteDecision & decision, const core::DagGraph & graph, const MaterializedPlan & materialized) {
    if (decision.source_readiness_topology_reused) return target_host_control_exact(decision, graph, materialized);
    if (!synthetic_rewrite(decision.rewrite_kind)) return true;
    const auto found = materialized.synthetic_nodes.find(decision.synthetic_id);
    if (found == materialized.synthetic_nodes.end()) return false;
    const auto node_id = found->second;
    const auto expected_lane = decision.resource_lane.empty() ? std::string_view{ "hicache_dependency" } : std::string_view{ decision.resource_lane };
    const bool effect_ready = graph.node(node_id).active && !graph.node(node_id).counts_toward_e2e && graph.node(node_id).duration == decision.duration_us
                              && graph.node_lane_key(node_id) == expected_lane;
    const bool host_control_ready = target_host_control_exact(decision, graph, materialized);
    if (!effect_ready || !host_control_ready || !decision.completion_join_required) return effect_ready && host_control_ready;
    const auto join = materialized.synthetic_nodes.find(decision.completion_join_synthetic_id);
    return join != materialized.synthetic_nodes.end() && graph.node(join->second).active && !graph.node(join->second).counts_toward_e2e
           && graph.node(join->second).duration == 0 && graph.node_lane_key(join->second) == "hicache_completion_join";
}

bool observable_endpoint_exact(const HiCacheRewriteDecision & decision, const core::DagGraph & graph, const MaterializedPlan & materialized) {
    const auto exact = [&](const auto & nodes) {
        return std::ranges::all_of(nodes, [&](size_t node_id) {
            return node_id < graph.node_count() && !graph.node(node_id).counts_toward_e2e && materialized.e2e_eligibility_updates.contains(node_id);
        });
    };
    return exact(decision.owned_duration_nodes) && exact(decision.source_completion_node_ids) && exact(decision.readiness_join_node_ids)
           && exact(decision.completion_wait_owned_node_ids);
}

bool ingress_exact(const HiCacheRewriteDecision & decision, const MaterializedPlan & materialized) {
    if (decision.source_readiness_topology_reused) {
        if (!decision.completion_join_contract_ready) return false;
        if (!decision.target_host_control_required) return true;
        const auto control = materialized.synthetic_nodes.find(decision.target_host_control_synthetic_id);
        return control != materialized.synthetic_nodes.end() && decision.target_host_control_anchor_node_id
               && (!decision.target_host_control_exit_node_ids.empty() || decision.effect_type == HiCacheEffectType::CommitDeviceToHost)
               && added_edge_exists(materialized, *decision.target_host_control_anchor_node_id, control->second, decision.effect_id)
               && std::ranges::all_of(decision.target_host_control_exit_node_ids,
                                      [&](size_t exit_node_id) { return added_edge_exists(materialized, control->second, exit_node_id, decision.effect_id); })
               && std::ranges::all_of(decision.owned_duration_nodes,
                                      [&](size_t node_id) { return added_edge_exists(materialized, control->second, node_id, decision.effect_id); });
    }
    if (!synthetic_rewrite(decision.rewrite_kind)) return true;
    const auto synthetic = materialized.synthetic_nodes.find(decision.synthetic_id);
    if (synthetic == materialized.synthetic_nodes.end()) return false;
    if (decision.target_host_control_required && !decision.target_host_control_terminal) {
        const auto control = materialized.synthetic_nodes.find(decision.target_host_control_synthetic_id);
        if (control == materialized.synthetic_nodes.end() || !decision.target_host_control_anchor_node_id
            || (decision.target_host_control_exit_node_ids.empty() && decision.effect_type != HiCacheEffectType::CommitDeviceToHost)
            || !added_edge_exists(materialized, *decision.target_host_control_anchor_node_id, control->second, decision.effect_id)
            || !std::ranges::all_of(decision.target_host_control_exit_node_ids,
                                    [&](size_t exit_node_id) { return added_edge_exists(materialized, control->second, exit_node_id, decision.effect_id); })
            || !added_edge_exists(materialized, control->second, synthetic->second, decision.effect_id))
            return false;
    }
    else if (!decision.source_execution_anchor_node_id
             || !added_edge_exists(materialized, *decision.source_execution_anchor_node_id, synthetic->second, decision.effect_id))
        return false;
    if (decision.completion_join_required && decision.control_ready_anchor_node_id == decision.wait_exit_anchor_node_id) {
        return decision.completion_control_ingress_edge_id && materialized.redirected_edges.contains(*decision.completion_control_ingress_edge_id);
    }
    if (insertion_rewrite(decision.rewrite_kind)) return true;
    if (!replacement_io(decision.rewrite_kind)) return true;
    return true;
}

bool consumer_exact(const HiCacheRewriteDecision & decision, const MaterializedPlan & materialized) {
    if (decision.source_readiness_topology_reused) {
        if (!decision.completion_join_contract_ready) return false;
        if (!decision.consumer_anchors.empty()) return true;
        const auto family_consumer = materialized.synthetic_nodes.find(decision.family_consumer_synthetic_id);
        if (decision.family_consumer_synthetic_id.empty() || family_consumer == materialized.synthetic_nodes.end()) return false;
        const auto & endpoints = decision.source_completion_node_ids.empty() ? decision.owned_duration_nodes : decision.source_completion_node_ids;
        return !endpoints.empty() && std::ranges::all_of(endpoints, [&](size_t endpoint) {
            return added_edge_exists(materialized, endpoint, family_consumer->second, "hicache_family_dependency");
        });
    }
    if (!synthetic_rewrite(decision.rewrite_kind)) return true;
    const auto synthetic = materialized.synthetic_nodes.find(decision.synthetic_id);
    if (synthetic == materialized.synthetic_nodes.end()) return false;
    if (decision.completion_join_required) {
        if (!decision.control_ready_anchor_node_id || !decision.wait_exit_anchor_node_id) return false;
        const auto join = materialized.synthetic_nodes.find(decision.completion_join_synthetic_id);
        if (join == materialized.synthetic_nodes.end() || !added_edge_exists(materialized, synthetic->second, join->second, decision.effect_id)) return false;
        if (decision.target_host_control_terminal) {
            const auto control = materialized.synthetic_nodes.find(decision.target_host_control_synthetic_id);
            if (control == materialized.synthetic_nodes.end() || !added_edge_exists(materialized, join->second, control->second, decision.effect_id)
                || !added_edge_exists(materialized, control->second, *decision.wait_exit_anchor_node_id, decision.effect_id))
                return false;
        }
        else if (!added_edge_exists(materialized, join->second, *decision.wait_exit_anchor_node_id, decision.effect_id)) return false;
        const bool control_ready = *decision.control_ready_anchor_node_id == *decision.wait_exit_anchor_node_id
                                       ? true
                                       : added_edge_exists(materialized, *decision.control_ready_anchor_node_id, join->second, decision.effect_id);
        return control_ready;
    }
    if (decision.target_host_control_terminal) {
        const auto terminal_join = materialized.synthetic_nodes.find(decision.target_host_control_terminal_join_synthetic_id);
        return terminal_join != materialized.synthetic_nodes.end()
               && added_edge_exists(materialized, synthetic->second, terminal_join->second, decision.effect_id);
    }
    if (decision.consumer_anchors.empty()) {
        if (!decision.request_consumer_synthetic_id.empty()) {
            const auto request_consumer = materialized.synthetic_nodes.find(decision.request_consumer_synthetic_id);
            return request_consumer != materialized.synthetic_nodes.end()
                   && added_edge_exists(materialized, synthetic->second, request_consumer->second, "hicache_request_io_dependency");
        }
        const auto family_consumer = materialized.synthetic_nodes.find(decision.family_consumer_synthetic_id);
        return !decision.family_consumer_synthetic_id.empty() && family_consumer != materialized.synthetic_nodes.end()
                   && added_edge_exists(materialized, synthetic->second, family_consumer->second, "hicache_family_dependency")
               || decision.effect_type == HiCacheEffectType::CommitDeviceToHost || decision.effect_type == HiCacheEffectType::CommitHostToStorage;
    }
    return std::ranges::all_of(decision.consumer_anchors,
                               [&](size_t consumer) { return added_edge_exists(materialized, synthetic->second, consumer, decision.effect_id); });
}

HiCacheAppliedEffectValidation validate_effect(const HiCacheRewriteDecision & decision, const core::DagGraph & graph, const MaterializedPlan & materialized) {
    HiCacheAppliedEffectValidation validation{
        .effect_id = decision.effect_id,
        .rewrite_kind = decision.rewrite_kind,
    };
    if (!decision.shadow_plan_ready || decision.rewrite_kind == HiCacheRewriteKind::Reject) {
        validation.reason = "effect was not ready in the complete shadow transaction";
        return validation;
    }
    if (decision.rewrite_kind == HiCacheRewriteKind::NoOp) {
        validation.source_duration_exact = true;
        validation.synthetic_cost_exact = true;
        validation.observable_endpoint_exact = true;
        validation.ingress_exact = true;
        validation.consumer_dependency_exact = true;
        validation.ready = true;
        validation.reason = "no-op preserves the source carrier and materializes no target mutation";
        return validation;
    }
    validation.source_duration_exact = duration_exact(decision, graph, materialized);
    validation.synthetic_cost_exact = synthetic_exact(decision, graph, materialized);
    validation.observable_endpoint_exact = observable_endpoint_exact(decision, graph, materialized);
    validation.ingress_exact = ingress_exact(decision, materialized);
    validation.consumer_dependency_exact = consumer_exact(decision, materialized);
    validation.ready = validation.source_duration_exact && validation.synthetic_cost_exact && validation.observable_endpoint_exact && validation.ingress_exact
                       && validation.consumer_dependency_exact;
    validation.reason = validation.ready ? "materialized effect matches its source-cost, target-cost, observable-endpoint, ingress, and consumer contract"
                                         : "materialized effect diverges from its validated shadow rewrite";
    return validation;
}

std::set<EdgeKey> planned_edges_by_effect(const core::DagMutationPlan & plan, const std::unordered_map<std::string, size_t> & synthetic_nodes,
                                          const std::set<std::string> & effect_ids) {
    std::set<EdgeKey> output;
    for (const auto & edge : plan.add_edges) {
        if (!effect_ids.contains(edge.effect_id)) continue;
        const auto src = resolve_ref(edge.src, synthetic_nodes);
        const auto dst = resolve_ref(edge.dst, synthetic_nodes);
        if (src && dst) output.emplace(*src, *dst, edge.kind, edge.effect_id);
    }
    return output;
}

std::unordered_map<std::string, size_t> synthetic_by_effect(const HiCacheShadowRewriteTransaction & shadow, const MaterializedPlan & materialized) {
    std::unordered_map<std::string, size_t> output;
    for (const auto & decision : shadow.decisions) {
        const auto found = materialized.synthetic_nodes.find(decision.synthetic_id);
        if (found != materialized.synthetic_nodes.end()) output.emplace(decision.effect_id, found->second);
    }
    return output;
}

std::set<EdgeKey> expected_family_edges(const HiCacheShadowRewriteTransaction & shadow, const MaterializedPlan & materialized,
                                        const std::unordered_map<std::string, size_t> & nodes_by_effect) {
    using FamilyEffects = std::map<HiCacheEffectType, std::string>;
    std::map<std::string, FamilyEffects> families;
    for (const auto & decision : shadow.decisions) {
        if (decision.effect_family_id.empty() || !nodes_by_effect.contains(decision.effect_id)) continue;
        families[decision.effect_family_id].emplace(decision.effect_type, decision.effect_id);
    }
    std::set<EdgeKey> output;
    const auto append = [&](const FamilyEffects & family, HiCacheEffectType predecessor, HiCacheEffectType successor) {
        const auto before = family.find(predecessor);
        const auto after = family.find(successor);
        if (before == family.end() || after == family.end()) return;
        output.emplace(nodes_by_effect.at(before->second), nodes_by_effect.at(after->second), core::DagEdgeKind::Mutation, "hicache_family_dependency");
    };
    for (const auto & family : families | std::views::values) {
        append(family, HiCacheEffectType::PrefetchIo, HiCacheEffectType::PrefetchVisibility);
        append(family, HiCacheEffectType::CommitDeviceToHost, HiCacheEffectType::CommitHostToStorage);
        append(family, HiCacheEffectType::CommitDeviceToHost, HiCacheEffectType::CommitCapacityGate);
    }
    for (const auto & decision : shadow.decisions) {
        if (!decision.source_readiness_topology_reused || decision.family_consumer_synthetic_id.empty()) continue;
        const auto consumer = materialized.synthetic_nodes.find(decision.family_consumer_synthetic_id);
        if (consumer == materialized.synthetic_nodes.end()) continue;
        const auto & endpoints = decision.source_completion_node_ids.empty() ? decision.owned_duration_nodes : decision.source_completion_node_ids;
        for (size_t endpoint : endpoints) output.emplace(endpoint, consumer->second, core::DagEdgeKind::Mutation, "hicache_family_dependency");
    }
    return output;
}

std::set<EdgeKey> expected_lane_edges(const HiCacheIoResourcePlan & resources, const std::unordered_map<std::string, size_t> & nodes_by_effect) {
    std::set<EdgeKey> output;
    for (const auto & dependency : resources.lane_dependencies) {
        const auto before = nodes_by_effect.find(dependency.predecessor_effect_id);
        const auto after = nodes_by_effect.find(dependency.successor_effect_id);
        if (before == nodes_by_effect.end() || after == nodes_by_effect.end()) continue;
        output.emplace(before->second, after->second, core::DagEdgeKind::Mutation, dependency.resource_lane);
    }
    return output;
}

} // namespace applied_validator_detail

uint64_t HiCacheAppliedPatchValidation::ready_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(records, [](const auto & record) { return record.ready; }));
}

HiCacheAppliedPatchValidation validate_hicache_applied_patch(const core::DagGraph & graph, const HiCacheShadowRewriteTransaction & shadow,
                                                             const HiCacheIoResourcePlan & resources, const core::DagMutationJournal & journal,
                                                             bool materialized_topology_valid) {
    HiCacheAppliedPatchValidation validation;
    auto materialized = applied_validator_detail::materialize_plan_index(graph, shadow.plan, journal, validation);
    validation.plan_journal_exact = materialized.ready;
    validation.prospective_materialization_exact =
        graph.active_node_count() == shadow.prospective_active_node_count && graph.active_edge_count() == shadow.prospective_active_edge_count
        && journal.active_nodes_after == graph.active_node_count() && journal.active_edges_after == graph.active_edge_count();
    if (!validation.prospective_materialization_exact) applied_validator_detail::add_blocker(validation, "prospective_materialized_count_mismatch");
    validation.topology_exact = shadow.topology_valid && materialized_topology_valid;
    if (!validation.topology_exact) applied_validator_detail::add_blocker(validation, "materialized_topology_invalid");

    validation.records.reserve(shadow.decisions.size());
    for (const auto & decision : shadow.decisions) {
        auto record = applied_validator_detail::validate_effect(decision, graph, materialized);
        if (!record.ready) applied_validator_detail::add_blocker(validation, "effect_materialization_mismatch");
        validation.records.push_back(std::move(record));
    }

    const auto nodes_by_effect = applied_validator_detail::synthetic_by_effect(shadow, materialized);
    const auto expected_family = applied_validator_detail::expected_family_edges(shadow, materialized, nodes_by_effect);
    const auto planned_family = applied_validator_detail::planned_edges_by_effect(shadow.plan, materialized.synthetic_nodes, { "hicache_family_dependency" });
    validation.family_dependencies_exact =
        expected_family == planned_family && std::ranges::all_of(expected_family, [&](const auto & edge) { return materialized.added_edges.contains(edge); });
    if (!validation.family_dependencies_exact) applied_validator_detail::add_blocker(validation, "family_dependency_materialization_mismatch");

    std::set<std::string> lane_ids;
    for (const auto & dependency : resources.lane_dependencies) lane_ids.insert(dependency.resource_lane);
    const auto expected_lanes = applied_validator_detail::expected_lane_edges(resources, nodes_by_effect);
    const auto planned_lanes = applied_validator_detail::planned_edges_by_effect(shadow.plan, materialized.synthetic_nodes, lane_ids);
    validation.lane_dependencies_exact =
        expected_lanes == planned_lanes && std::ranges::all_of(expected_lanes, [&](const auto & edge) { return materialized.added_edges.contains(edge); });
    if (!validation.lane_dependencies_exact) applied_validator_detail::add_blocker(validation, "lane_dependency_materialization_mismatch");

    if (!validation.plan_journal_exact) applied_validator_detail::add_blocker(validation, "plan_journal_mismatch");
    validation.status = validation.blocker_counts.empty() && validation.ready_count() == validation.records.size() ? "ready" : "failed";
    return validation;
}

} // namespace markov::trace_graph::modules::hicache::patch
