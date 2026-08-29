#include "rewrite_decision.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail {

using model::HiCacheEffectDecision;
using model::HiCacheEffectType;
using model::HiCacheSourceCarrierState;
using model::HiCacheTargetEffectState;
using model::HiCacheTransferDirection;

bool dependency_effect(const HiCacheEffectDecision & effect) { return effect.direction == HiCacheTransferDirection::None; }

bool background_write_effect(const HiCacheEffectDecision & effect) {
    return effect.effect_type == HiCacheEffectType::CommitDeviceToHost || effect.effect_type == HiCacheEffectType::CommitHostToStorage;
}

bool foreground_io_effect(const HiCacheEffectDecision & effect) {
    return effect.effect_type == HiCacheEffectType::PrefetchIo || effect.effect_type == HiCacheEffectType::Loadback;
}

HiCacheRewriteDecision reject_decision(const HiCacheEffectDecision & effect, const HiCacheSourceAttribution * attribution, std::string blocker) {
    return HiCacheRewriteDecision{
        .effect_id = effect.effect_key,
        .effect_family_id = effect.effect_family_key,
        .cache_scope = effect.cache_scope,
        .request_id = effect.request_id_provenance,
        .eligibility_timestamp_us = effect.eligibility_boundary.timestamp_us,
        .effect_type = effect.effect_type,
        .target_effect_state = effect.target_effect_state,
        .source_carrier_state = attribution == nullptr ? HiCacheSourceCarrierState::NotEvaluated : attribution->source_carrier_state,
        .rewrite_kind = HiCacheRewriteKind::Reject,
        .source_fact_node_id = effect.source_node_id,
        .source_execution_anchor_node_id = effect.source_execution_anchor_node_id,
        .reason = "rewrite is not safe under the current source/target evidence",
        .blocker = std::move(blocker),
    };
}

HiCacheRewriteDecision classify(const HiCacheEffectDecision & effect, const HiCacheSourceAttribution * attribution, const HiCacheIoCostRecord * cost,
                                bool source_target_same_config) {
    if (source_target_same_config) {
        return HiCacheRewriteDecision{
            .effect_id = effect.effect_key,
            .effect_family_id = effect.effect_family_key,
            .cache_scope = effect.cache_scope,
            .request_id = effect.request_id_provenance,
            .eligibility_timestamp_us = effect.eligibility_boundary.timestamp_us,
            .effect_type = effect.effect_type,
            .target_effect_state = effect.target_effect_state,
            .source_carrier_state = attribution == nullptr ? HiCacheSourceCarrierState::NotEvaluated : attribution->source_carrier_state,
            .rewrite_kind = HiCacheRewriteKind::NoOp,
            .shadow_plan_ready = true,
            .source_fact_node_id = effect.source_node_id,
            .source_execution_anchor_node_id = effect.source_execution_anchor_node_id,
            .reason = "source and target configurations are identical; preserve the faithful source topology",
        };
    }
    if (attribution == nullptr) return reject_decision(effect, nullptr, "missing_source_attribution");
    if (effect.target_effect_state == HiCacheTargetEffectState::Deferred) return reject_decision(effect, attribution, "target_effect_deferred");
    if (effect.target_effect_state == HiCacheTargetEffectState::Unresolved) return reject_decision(effect, attribution, "target_effect_unresolved");
    if (attribution->source_carrier_state == HiCacheSourceCarrierState::NotEvaluated)
        return reject_decision(effect, attribution, "source_carrier_not_evaluated");
    if (attribution->source_carrier_state == HiCacheSourceCarrierState::Unobservable)
        return reject_decision(effect, attribution, "source_carrier_unobservable:" + attribution->reason);
    if (attribution->source_carrier_state == HiCacheSourceCarrierState::Ambiguous)
        return reject_decision(effect, attribution, "source_carrier_ambiguous:" + attribution->reason);

    const auto source_execution_anchor =
        effect.source_execution_anchor_node_id ? effect.source_execution_anchor_node_id : attribution->source_execution_anchor_node_id;
    const bool reuse_source_readiness_topology =
        (effect.effect_type == HiCacheEffectType::Loadback || effect.effect_type == HiCacheEffectType::CommitDeviceToHost)
        && attribution->source_carrier_state == HiCacheSourceCarrierState::Present && attribution->source_readiness_topology_ready
        && attribution->source_effect_schedule_aligned
        && (effect.effect_type != HiCacheEffectType::CommitDeviceToHost || effect.target_effect_state != HiCacheTargetEffectState::NotRequired)
        && !attribution->owned_duration_nodes.empty();
    HiCacheRewriteDecision decision{
        .effect_id = effect.effect_key,
        .effect_family_id = effect.effect_family_key,
        .cache_scope = effect.cache_scope,
        .request_id = effect.request_id_provenance,
        .eligibility_timestamp_us = effect.eligibility_boundary.timestamp_us,
        .effect_type = effect.effect_type,
        .target_effect_state = effect.target_effect_state,
        .source_carrier_state = attribution->source_carrier_state,
        .duration_us = cost == nullptr ? 0 : cost->duration_us,
        .resource_lane = cost == nullptr ? std::string{} : cost->resource_lane,
        .synthetic_id = "hicache_effect:" + effect.effect_key,
        .carrier_nodes = attribution->carrier_nodes,
        .owned_duration_nodes = attribution->owned_duration_nodes,
        .owned_gap_slices = attribution->owned_gap_slices,
        .source_control_duration_nodes = attribution->source_control_duration_nodes,
        .source_control_gap_slices = attribution->source_control_gap_slices,
        .source_gap_removal_slices = attribution->source_gap_removal_slices,
        .logical_input_causal_gap_slices = attribution->logical_input_causal_gap_slices,
        .observed_io_duration_us = attribution->observed_io_duration_us,
        .owned_gap_duration_us = attribution->owned_gap_duration_us,
        .target_host_control_duration_us = cost == nullptr ? 0 : cost->host_control_duration_us,
        .target_host_control_required =
            cost != nullptr && cost->host_control_duration_us > 0
            && (cost->zero_payload_control
                || ((effect.effect_type == HiCacheEffectType::Loadback || effect.effect_type == HiCacheEffectType::CommitDeviceToHost)
                    && effect.target_effect_state != HiCacheTargetEffectState::NotRequired)),
        .target_host_control_terminal = cost != nullptr && cost->zero_payload_control,
        .target_host_control_synthetic_id = "hicache_host_control:" + effect.effect_key,
        .target_host_control_terminal_join_synthetic_id = "hicache_terminal_control_join:" + effect.effect_key,
        .target_host_control_anchor_node_id = effect.effect_type == HiCacheEffectType::CommitDeviceToHost
                                                  ? source_execution_anchor
                                                  : (attribution->start_anchor ? attribution->start_anchor : source_execution_anchor),
        .target_host_control_exit_node_id = attribution->completion_anchor ? attribution->completion_anchor : attribution->control_ready_anchor_node_id,
        .target_host_control_exit_node_ids =
            [&] {
                std::vector<size_t> exits;
                exits.reserve(attribution->source_control_gap_slices.size());
                for (const auto & gap : attribution->source_control_gap_slices) exits.push_back(gap.successor_node_id);
                std::ranges::sort(exits);
                exits.erase(std::unique(exits.begin(), exits.end()), exits.end());
                return exits;
            }(),
        .target_host_control_ingress_edge_id = std::nullopt,
        .target_host_control_ingress_edge_ids = {},
        .source_gap_removal_duration_us = attribution->source_gap_removal_duration_us,
        .logical_input_causal_gap_duration_us = attribution->logical_input_causal_gap_duration_us,
        .residual_unknown_duration_us = attribution->residual_unknown_duration_us,
        .observed_span_semantics = attribution->observed_span_semantics,
        .completion_wait_status = attribution->completion_wait_status,
        .completion_wait_reason = attribution->completion_wait_reason,
        .completion_join_contract_ready = attribution->completion_join_contract_ready,
        .completion_join_required =
            (effect.effect_type == HiCacheEffectType::PrefetchIo || (effect.effect_type == HiCacheEffectType::Loadback && !reuse_source_readiness_topology))
            && attribution->completion_join_contract_ready
            && (attribution->source_carrier_state == HiCacheSourceCarrierState::Present || effect.target_effect_state != HiCacheTargetEffectState::NotRequired),
        .source_effect_schedule_aligned = attribution->source_effect_schedule_aligned,
        .source_readiness_topology_reused = reuse_source_readiness_topology,
        .source_completion_wait_blocking = attribution->source_completion_wait_blocking,
        .source_control_removal_required = attribution->source_control_removal_required,
        .completion_join_synthetic_id = "hicache_io_completion_join:" + effect.effect_key,
        .control_ready_us = attribution->control_ready_us,
        .source_completion_us = attribution->source_completion_us,
        .wait_exit_start_us = attribution->wait_exit_start_us,
        .wait_exit_end_us = attribution->wait_exit_end_us,
        .completion_wait_duration_us = attribution->completion_wait_duration_us,
        .completion_wait_gap_duration_us = attribution->completion_wait_gap_duration_us,
        .logical_input_completion_wait_duration_us = attribution->logical_input_completion_wait_duration_us,
        .polling_lag_us = attribution->polling_lag_us,
        .retained_terminal_control_us = attribution->retained_terminal_control_us,
        .control_ready_anchor_node_id = attribution->control_ready_anchor_node_id,
        .wait_exit_anchor_node_id = attribution->wait_exit_anchor_node_id,
        .terminal_control_anchor_node_id = attribution->terminal_control_anchor_node_id,
        .completion_wait_owned_node_ids = attribution->completion_wait_owned_node_ids,
        .source_completion_node_ids = attribution->source_completion_node_ids,
        .readiness_join_node_ids = attribution->readiness_join_node_ids,
        .completion_wait_slices = attribution->completion_wait_slices,
        .logical_input_completion_wait_slices = attribution->logical_input_completion_wait_slices,
        .carrier_entry_edges = attribution->carrier_entry_edges,
        .carrier_exit_edges = attribution->carrier_exit_edges,
        .source_fact_node_id = effect.source_node_id,
        .source_execution_anchor_node_id = source_execution_anchor,
        .consumer_anchors = attribution->consumer_anchors,
        .consumer_anchor_method = attribution->consumer_anchor_method,
    };

    if (decision.target_host_control_terminal) {
        // A zero-payload target has no storage completion to join against.  Its
        // only target-side cost is the terminal progress/control operation at
        // the real consumer boundary; retaining the source completion join
        // would preserve source-only wait topology and mask that control cost.
        decision.completion_join_required = false;
    }

    const bool equivalent_prefetch_completion = effect.effect_type == HiCacheEffectType::PrefetchIo
                                                && attribution->source_carrier_state == HiCacheSourceCarrierState::Present
                                                && effect.target_effect_state != HiCacheTargetEffectState::NotRequired
                                                && attribution->source_completed_token_count == attribution->target_effective_token_count;
    if (equivalent_prefetch_completion) {
        decision.completion_join_required = false;
        decision.rewrite_kind = HiCacheRewriteKind::NoOp;
        decision.shadow_plan_ready = true;
        decision.reason = "source and target prefetch completion semantics are identical; preserve the faithful source wait topology";
        return decision;
    }

    if (attribution->source_carrier_state == HiCacheSourceCarrierState::Absent) {
        if (effect.target_effect_state == HiCacheTargetEffectState::NotRequired) {
            const bool zero_length_io = effect.effect_type == HiCacheEffectType::PrefetchIo || effect.effect_type == HiCacheEffectType::Loadback;
            if (!zero_length_io) {
                decision.rewrite_kind = HiCacheRewriteKind::NoOp;
                decision.shadow_plan_ready = true;
                decision.reason = "source and target both explicitly omit this effect";
                return decision;
            }
            if (!decision.source_execution_anchor_node_id || attribution->consumer_anchors.empty()) {
                decision.rewrite_kind = HiCacheRewriteKind::NoOp;
                decision.shadow_plan_ready = true;
                decision.reason = "source and target omit the transfer and no executable consumer boundary exists";
                return decision;
            }
            decision.duration_us = 0;
            decision.rewrite_kind = HiCacheRewriteKind::InsertIo;
            decision.shadow_plan_ready = true;
            decision.reason = "target retains a zero-length I/O boundary for an omitted effective transfer";
            return decision;
        }
        if (cost == nullptr || cost->status != HiCacheIoCostStatus::Ready) return reject_decision(effect, attribution, "target_effect_cost_not_ready");
        if (foreground_io_effect(effect) && !decision.completion_join_contract_ready)
            return reject_decision(effect, attribution, "foreground_io_completion_contract_not_ready:" + attribution->completion_wait_reason);
        if (!decision.source_execution_anchor_node_id) return reject_decision(effect, attribution, "missing_source_execution_anchor");
        if (attribution->consumer_anchors.empty() && !background_write_effect(effect))
            return reject_decision(effect, attribution, "missing_insertion_consumer_anchor");
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
        const bool preserved_loadback_submission =
            effect.effect_type == HiCacheEffectType::Loadback && attribution->observed_span_semantics == "host_submission";
        if (attribution->owned_duration_nodes.empty() && attribution->owned_gap_slices.empty() && !decision.completion_join_required
            && !preserved_loadback_submission)
            return reject_decision(effect, attribution, "present_source_carrier_has_no_owned_duration");
        if (effect.effect_type == HiCacheEffectType::PrefetchIo || effect.effect_type == HiCacheEffectType::Loadback) {
            if (foreground_io_effect(effect) && !decision.completion_join_contract_ready)
                return reject_decision(effect, attribution, "foreground_io_completion_contract_not_ready:" + attribution->completion_wait_reason);
            if (!decision.source_execution_anchor_node_id) return reject_decision(effect, attribution, "missing_source_execution_anchor");
            if (attribution->consumer_anchors.empty()) return reject_decision(effect, attribution, "missing_replacement_consumer_anchor");
            decision.duration_us = 0;
            decision.rewrite_kind = HiCacheRewriteKind::ReplaceWithIo;
            decision.shadow_plan_ready = true;
            decision.reason = "target retains the source causal topology with a zero-length effective transfer";
            return decision;
        }
        decision.rewrite_kind = HiCacheRewriteKind::RemoveOwnedCost;
        decision.shadow_plan_ready = true;
        decision.reason = "target explicitly removes the source effect while retaining its boundary";
        return decision;
    }
    if (cost == nullptr || cost->status != HiCacheIoCostStatus::Ready) return reject_decision(effect, attribution, "target_effect_cost_not_ready");
    if (effect.target_effect_state == HiCacheTargetEffectState::Partial) {
        if (dependency_effect(effect)) return reject_decision(effect, attribution, "partial_dependency_effect_is_invalid");
        const bool preserved_loadback_submission =
            effect.effect_type == HiCacheEffectType::Loadback && attribution->observed_span_semantics == "host_submission";
        if (attribution->owned_duration_nodes.empty() && attribution->owned_gap_slices.empty() && !decision.completion_join_required
            && !preserved_loadback_submission)
            return reject_decision(effect, attribution, "present_source_carrier_has_no_owned_duration");
        if (foreground_io_effect(effect) && !decision.completion_join_contract_ready)
            return reject_decision(effect, attribution, "foreground_io_completion_contract_not_ready:" + attribution->completion_wait_reason);
        if (!decision.source_execution_anchor_node_id) return reject_decision(effect, attribution, "missing_source_execution_anchor");
        if (attribution->consumer_anchors.empty() && !background_write_effect(effect))
            return reject_decision(effect, attribution, "missing_replacement_consumer_anchor");
        decision.rewrite_kind = HiCacheRewriteKind::PartialReplace;
        decision.shadow_plan_ready = true;
        decision.reason = "source timing is effect-local and can be replaced completely by the target-derived partial transfer cost";
        return decision;
    }
    if (dependency_effect(effect)) {
        if (!decision.source_execution_anchor_node_id) return reject_decision(effect, attribution, "missing_source_execution_anchor");
        if (attribution->consumer_anchors.empty()) return reject_decision(effect, attribution, "missing_dependency_consumer_anchor");
        decision.rewrite_kind = HiCacheRewriteKind::ReplaceWithGate;
    }
    else {
        const bool preserved_loadback_submission =
            effect.effect_type == HiCacheEffectType::Loadback && attribution->observed_span_semantics == "host_submission";
        if (attribution->owned_duration_nodes.empty() && attribution->owned_gap_slices.empty() && !decision.completion_join_required
            && !preserved_loadback_submission)
            return reject_decision(effect, attribution, "present_source_carrier_has_no_owned_duration");
        if (foreground_io_effect(effect) && !decision.completion_join_contract_ready)
            return reject_decision(effect, attribution, "foreground_io_completion_contract_not_ready:" + attribution->completion_wait_reason);
        if (!decision.source_execution_anchor_node_id) return reject_decision(effect, attribution, "missing_source_execution_anchor");
        if (attribution->consumer_anchors.empty() && !background_write_effect(effect))
            return reject_decision(effect, attribution, "missing_replacement_consumer_anchor");
        decision.rewrite_kind = HiCacheRewriteKind::ReplaceWithIo;
    }
    decision.shadow_plan_ready = true;
    decision.reason = "source carrier can be replaced by the target-derived effect cost";
    return decision;
}


} // namespace markov::trace_graph::modules::hicache::patch::rewrite_transaction_detail
