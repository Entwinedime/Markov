#include "rewrite_boundary.hpp"

#include <algorithm>
#include <map>
#include <ranges>

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

using model::HiCacheEffectType;

std::optional<size_t> unique_sequential_ingress(const core::DagGraph & graph, size_t node_id) {
    std::optional<size_t> match;
    for (size_t edge_index = 0; edge_index < graph.edge_count(); ++edge_index) {
        const auto & edge = graph.edge(edge_index);
        if (!edge.active || edge.dst != node_id || edge.kind != core::DagEdgeKind::Sequential) continue;
        if (match) return std::nullopt;
        match = edge_index;
    }
    return match;
}

std::optional<size_t> unique_sequential_egress_node(const core::DagGraph & graph, size_t node_id) {
    std::optional<size_t> match;
    for (size_t edge_index = 0; edge_index < graph.edge_count(); ++edge_index) {
        const auto & edge = graph.edge(edge_index);
        if (!edge.active || edge.src != node_id || edge.kind != core::DagEdgeKind::Sequential) continue;
        if (match && *match != edge.dst) return std::nullopt;
        match = edge.dst;
    }
    return match;
}

void resolve_target_host_control_boundaries(const core::DagGraph & graph, std::vector<HiCacheRewriteDecision> & decisions) {
    for (auto & decision : decisions) {
        if (!decision.target_host_control_required || !decision.shadow_plan_ready) continue;
        if (decision.target_host_control_terminal) {
            decision.target_host_control_exit_node_ids.clear();
            if (decision.consumer_anchors.size() == 1) decision.target_host_control_exit_node_ids.push_back(decision.consumer_anchors.front());
            else if (decision.terminal_control_anchor_node_id) decision.target_host_control_exit_node_ids.push_back(*decision.terminal_control_anchor_node_id);
            else if (decision.wait_exit_anchor_node_id) decision.target_host_control_exit_node_ids.push_back(*decision.wait_exit_anchor_node_id);
            else if (decision.target_host_control_exit_node_id)
                decision.target_host_control_exit_node_ids.push_back(*decision.target_host_control_exit_node_id);
            if (decision.target_host_control_exit_node_ids.size() != 1) {
                decision.rewrite_kind = HiCacheRewriteKind::Reject;
                decision.shadow_plan_ready = false;
                decision.blocker = "target_terminal_control_exit_ambiguous";
                decision.reason = "zero-payload terminal control does not resolve one executable completion/consumer boundary";
                continue;
            }
            decision.target_host_control_exit_node_id = decision.target_host_control_exit_node_ids.front();
            decision.target_host_control_ingress_edge_ids.clear();
            for (size_t edge_index = 0; edge_index < graph.edge_count(); ++edge_index) {
                const auto & edge = graph.edge(edge_index);
                if (edge.active && edge.dst == decision.target_host_control_exit_node_ids.front())
                    decision.target_host_control_ingress_edge_ids.push_back(edge_index);
            }
            decision.target_host_control_ingress_edge_id = unique_sequential_ingress(graph, decision.target_host_control_exit_node_ids.front());
            if (decision.target_host_control_ingress_edge_ids.empty()) {
                decision.rewrite_kind = HiCacheRewriteKind::Reject;
                decision.shadow_plan_ready = false;
                decision.blocker = "target_terminal_control_ingress_missing";
                decision.reason = "zero-payload terminal control consumer has no executable readiness ingress";
                continue;
            }
            decision.target_host_control_anchor_node_id = decision.target_host_control_ingress_edge_id
                                                              ? std::optional<size_t>{ graph.edge(*decision.target_host_control_ingress_edge_id).src }
                                                              : std::nullopt;
            continue;
        }
        if (!decision.target_host_control_anchor_node_id) {
            decision.rewrite_kind = HiCacheRewriteKind::Reject;
            decision.shadow_plan_ready = false;
            decision.blocker = "target_host_control_anchor_missing";
            decision.reason = "target host-control has no executable opportunity boundary";
            continue;
        }
        if (decision.effect_type == HiCacheEffectType::CommitDeviceToHost) {
            decision.target_host_control_exit_node_ids.clear();
            decision.target_host_control_exit_node_id = std::nullopt;
        }
        decision.target_host_control_exit_node_ids.erase(std::remove(decision.target_host_control_exit_node_ids.begin(),
                                                                     decision.target_host_control_exit_node_ids.end(),
                                                                     *decision.target_host_control_anchor_node_id),
                                                         decision.target_host_control_exit_node_ids.end());
        if (decision.target_host_control_exit_node_ids.empty()) {
            if (decision.target_host_control_exit_node_id && decision.target_host_control_exit_node_id != decision.target_host_control_anchor_node_id)
                decision.target_host_control_exit_node_ids.push_back(*decision.target_host_control_exit_node_id);
            else if (const auto successor = unique_sequential_egress_node(graph, *decision.target_host_control_anchor_node_id))
                decision.target_host_control_exit_node_ids.push_back(*successor);
        }
        std::ranges::sort(decision.target_host_control_exit_node_ids);
        decision.target_host_control_exit_node_ids.erase(
            std::unique(decision.target_host_control_exit_node_ids.begin(), decision.target_host_control_exit_node_ids.end()),
            decision.target_host_control_exit_node_ids.end());
        if (!decision.target_host_control_exit_node_ids.empty() || decision.effect_type == HiCacheEffectType::CommitDeviceToHost) continue;
        decision.rewrite_kind = HiCacheRewriteKind::Reject;
        decision.shadow_plan_ready = false;
        decision.blocker = "target_host_control_exit_missing";
        decision.reason = "target host-control has no executable CPU continuation boundary";
    }
}

void validate_completion_join_boundaries(const core::DagGraph & graph, std::vector<HiCacheRewriteDecision> & decisions) {
    for (auto & decision : decisions) {
        if (!decision.completion_join_required || !decision.shadow_plan_ready) continue;
        if (!decision.source_execution_anchor_node_id || !decision.control_ready_anchor_node_id || !decision.wait_exit_anchor_node_id
            || !decision.terminal_control_anchor_node_id) {
            decision.rewrite_kind = HiCacheRewriteKind::Reject;
            decision.shadow_plan_ready = false;
            decision.blocker = "completion_join_missing_executable_boundary";
            decision.reason = "completion-join contract does not resolve every executable boundary";
            decision.completion_join_required = false;
            continue;
        }
        if (*decision.control_ready_anchor_node_id != *decision.wait_exit_anchor_node_id) continue;
        decision.completion_control_ingress_edge_id = unique_sequential_ingress(graph, *decision.control_ready_anchor_node_id);
        if (decision.completion_control_ingress_edge_id) continue;
        decision.rewrite_kind = HiCacheRewriteKind::Reject;
        decision.shadow_plan_ready = false;
        decision.blocker = "completion_join_immediate_ready_ingress_ambiguous";
        decision.reason = "immediate-ready completion join does not have one sequential CPU control ingress";
        decision.completion_join_required = false;
    }
}

void fold_shared_immediate_ready_completion_joins(std::vector<HiCacheRewriteDecision> & decisions) {
    std::map<size_t, std::vector<HiCacheRewriteDecision *>> by_ingress;
    for (auto & decision : decisions) {
        if (decision.completion_join_required && decision.completion_control_ingress_edge_id)
            by_ingress[*decision.completion_control_ingress_edge_id].push_back(&decision);
    }
    for (auto & shared : by_ingress | std::views::values) {
        if (shared.size() != 2) continue;
        HiCacheRewriteDecision * prefetch = nullptr;
        HiCacheRewriteDecision * loadback = nullptr;
        for (auto * decision : shared) {
            if (decision->effect_type == HiCacheEffectType::PrefetchIo) prefetch = decision;
            else if (decision->effect_type == HiCacheEffectType::Loadback) loadback = decision;
        }
        if (prefetch == nullptr || loadback == nullptr || prefetch->request_id.empty() || prefetch->request_id != loadback->request_id) continue;
        prefetch->completion_join_required = false;
        prefetch->completion_control_ingress_edge_id = std::nullopt;
        prefetch->consumer_anchors.clear();
        prefetch->consumer_anchor_method = "request_loadback_completion_join";
        prefetch->request_consumer_synthetic_id = loadback->synthetic_id;
        prefetch->reason = "prefetch completion feeds the request loadback; one shared immediate-ready completion join guards the final consumer";
    }
}


} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
