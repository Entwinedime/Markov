/**
 * @file
 * @brief Prefetch and Load request I/O ledger attribution.
 */
#include "attribution_common.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <span>

namespace markov::trace_graph::modules::hicache::patch::attribution_detail {

using model::HiCacheEffectDecision;
using model::HiCacheEffectType;
using model::HiCacheSourceCarrierState;

namespace {

bool same_process(const HiCacheSourceFactNode & fact, const HiCacheSourceFactNode & anchor) { return fact.pid == anchor.pid; }

const HiCacheSourceFactNode * next_request_opportunity(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision,
                                                       const HiCacheSourceFactNode & anchor) {
    const HiCacheSourceFactNode * next = nullptr;
    if (decision.request_id_provenance.empty()) return next;
    for (size_t node_id : source.nodes_for_request(decision.request_id_provenance)) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate == nullptr || candidate->node_id == anchor.node_id || !same_process(*candidate, anchor) || candidate->fact_role != anchor.fact_role
            || !fact_precedes(anchor, *candidate))
            continue;
        if (next == nullptr || fact_precedes(*candidate, *next)) next = candidate;
    }
    return next;
}

std::optional<HiCacheIoOperationKind> operation_kind_for_effect(HiCacheEffectType effect_type) {
    switch (effect_type) {
    case HiCacheEffectType::PrefetchIo:
        return HiCacheIoOperationKind::Prefetch;
    case HiCacheEffectType::Loadback:
        return HiCacheIoOperationKind::Load;
    case HiCacheEffectType::CommitDeviceToHost:
        return HiCacheIoOperationKind::WriteDeviceToHost;
    case HiCacheEffectType::CommitHostToStorage:
        return HiCacheIoOperationKind::WriteHostToStorage;
    case HiCacheEffectType::PrefetchVisibility:
    case HiCacheEffectType::CommitCapacityGate:
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<HiCacheCpuGapSlice> proportional_tail_removal(std::span<const HiCacheCpuGapSlice> source_slices, uint64_t removal_tokens, uint64_t source_tokens) {
    std::vector<HiCacheCpuGapSlice> output;
    if (removal_tokens == 0 || source_tokens == 0) return output;
    output.reserve(source_slices.size());
    for (const auto & slice : source_slices) {
        const auto removal_duration = core::ceil_multiply_divide_u64(slice.owned_duration_us(), removal_tokens, source_tokens);
        if (!removal_duration || *removal_duration == 0) continue;
        auto removal = slice;
        removal.owned_start_us = removal.owned_end_us - std::min(*removal_duration, removal.owned_duration_us());
        output.push_back(removal);
    }
    return output;
}

} // namespace

void classify_io_from_ledger(const HiCacheSourceDagIndex & source, const HiCacheIoOperationLedger & operations, const HiCacheEffectDecision & decision,
                             const HiCacheSourceFactNode & anchor, HiCacheSourceAttribution & output) {
    const auto kind = operation_kind_for_effect(decision.effect_type);
    if (!kind) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.reason = "effect has no source I/O operation-ledger kind";
        return;
    }
    std::vector<const HiCacheIoOperationRecord *> matches;
    const auto * next_opportunity = next_request_opportunity(source, decision, anchor);
    const auto opportunity_start = fact_boundary(anchor);
    const auto opportunity_end = next_opportunity == nullptr ? std::numeric_limits<uint64_t>::max() : fact_boundary(*next_opportunity);
    for (const auto & operation : operations.records) {
        if (operation.kind != *kind || operation.pid != anchor.pid) continue;
        if (!decision.request_id_provenance.empty() && operation.request_id != decision.request_id_provenance) continue;
        if (operation.source_start_us < opportunity_start || operation.source_start_us >= opportunity_end) continue;
        matches.push_back(&operation);
    }
    if (matches.empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.reason = "source operation ledger proves no I/O operation for this opportunity";
        if ((decision.effect_type == HiCacheEffectType::PrefetchIo || decision.effect_type == HiCacheEffectType::Loadback)
            && output.consumer_anchors.size() == 1) {
            const auto consumer = output.consumer_anchors.front();
            if (consumer < source.graph().node_count() && source.graph().node(consumer).active && source.graph().node(consumer).is_cpu) {
                const auto & event = source.graph().event_for_node(consumer);
                output.observed_span_semantics = "operation_absent";
                output.completion_wait_status = "ready";
                output.completion_wait_reason =
                    "source has no foreground I/O operation; the canonical consumer sequential ingress is the immediate-ready control branch";
                output.completion_join_contract_ready = true;
                output.control_ready_us = event.ts;
                output.wait_exit_start_us = event.ts;
                output.wait_exit_end_us = fact_end(HiCacheSourceFactNode{ .timestamp_us = event.ts, .duration_us = event.dur });
                output.retained_terminal_control_us = event.dur;
                output.control_ready_anchor_node_id = consumer;
                output.wait_exit_anchor_node_id = consumer;
                output.terminal_control_anchor_node_id = consumer;
                output.evidence.push_back("foreground_io_absent_canonical_consumer_control_boundary");
            }
        }
        return;
    }
    if (matches.size() != 1) {
        output.source_carrier_state = HiCacheSourceCarrierState::Ambiguous;
        output.reason = "multiple nonzero source I/O ledger records match one opportunity";
        return;
    }
    const auto & operation = *matches.front();
    output.io_operation_record_ids.push_back(operation.record_id);
    copy_completion_wait_contract(operation, output);
    output.timing_fact_nodes.push_back(operation.timing_fact_node_id);
    output.control_fact_nodes.insert(output.control_fact_nodes.end(), operation.control_fact_node_ids.begin(), operation.control_fact_node_ids.end());
    output.operation_chain_nodes = operation.control_fact_node_ids;
    if (operation.status != "ready") {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.reason = operation.reason;
        return;
    }
    output.source_completed_token_count = operation.completed_token_count;
    output.target_effective_token_count = target_effective_token_count(decision);
    output.observed_io_duration_us = operation.observed_duration_us;
    output.residual_unknown_duration_us = operation.observed_duration_us;
    output.evidence.insert(output.evidence.end(), operation.evidence.begin(), operation.evidence.end());
    if (operation.completed_token_count == 0) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.reason = "source operation ledger proves an explicit zero-payload I/O boundary";
        return;
    }
    if (!operation.runtime_node_ids.empty()) {
        assign_carrier_nodes(source,
                             operation.runtime_node_ids,
                             "source I/O timing interval owns exact retained runtime leaves and CPU gaps",
                             output);
    }
    else {
        output.source_carrier_state = HiCacheSourceCarrierState::Present;
        output.reason = "source I/O timing is represented by a proven causal foreground CPU gap";
    }
    if (operation.kind == HiCacheIoOperationKind::Prefetch && operation.completion_join_contract_ready) {
        output.owned_duration_nodes = operation.completion_wait_owned_node_ids;
        output.owned_gap_slices.clear();
        output.source_gap_removal_slices.clear();
        output.logical_input_causal_gap_slices.clear();
        output.owned_gap_duration_us = 0;
        output.source_gap_removal_duration_us = 0;
        output.logical_input_causal_gap_duration_us = 0;
        output.evidence.push_back("completion_join_replaces_payload_ratio_gap_timing");
    }
    else if (operation.kind == HiCacheIoOperationKind::Load && operation.source_readiness_topology_ready) {
        output.owned_duration_nodes = operation.device_transfer_node_ids;
        output.owned_gap_slices.clear();
        output.owned_gap_duration_us = 0;
        output.residual_unknown_duration_us = operation.observed_duration_us;
        output.evidence.push_back("loadback_host_submission_duration_preserved");
        output.evidence.push_back("loadback_device_transfer_duration_owned");
        output.evidence.push_back("loadback_existing_event_join_preserved");
        append_snapshot_isolated_control_ownership(
            source,
            source.timing_interval_ownership(operation.pid, operation.tid, operation.source_start_us, operation.observed_duration_us),
            operation.pid,
            operation.tid,
            output);
        output.source_control_duration_nodes.insert(output.source_control_duration_nodes.end(),
                                                    operation.admission_explicit_node_ids.begin(),
                                                    operation.admission_explicit_node_ids.end());
        output.source_control_duration_nodes.insert(output.source_control_duration_nodes.end(),
                                                    operation.admission_python_self_node_ids.begin(),
                                                    operation.admission_python_self_node_ids.end());
        output.source_control_gap_slices.insert(output.source_control_gap_slices.end(),
                                                operation.admission_cpu_gap_slices.begin(),
                                                operation.admission_cpu_gap_slices.end());
        finalize_source_control_ownership(source, output);
        output.source_control_removal_required = !output.source_control_duration_nodes.empty() || !output.source_control_gap_slices.empty();
        if (output.source_control_removal_required) {
            output.evidence.push_back("source_loadback_host_control_fully_owned");
            output.evidence.push_back("source_host_control_snapshot_excluded");
            output.evidence.push_back("source_host_control_logical_input_gap_projection");
        }
    }
    else {
        output.owned_gap_slices = operation.cpu_gap_slices;
        output.owned_gap_duration_us = operation.owned_gap_duration_us;
    }
    if (!operation.completion_join_contract_ready && operation.foreground_consumer_required
        && output.target_effective_token_count < output.source_completed_token_count && !operation.cpu_gap_slices.empty()) {
        const auto removed_token_count = output.source_completed_token_count - output.target_effective_token_count;
        output.source_gap_removal_slices = proportional_tail_removal(operation.cpu_gap_slices, removed_token_count, output.source_completed_token_count);
        for (const auto & slice : output.source_gap_removal_slices) {
            output.source_gap_removal_duration_us = core::checked_add_u64(output.source_gap_removal_duration_us,
                                                                          slice.owned_duration_us(),
                                                                          "HiCache source CPU gap removal duration exceeds uint64 range");
        }
        output.logical_input_causal_gap_slices = source.project_foreground_gap_across_logical_input_lanes(output.source_gap_removal_slices);
        for (const auto & slice : output.logical_input_causal_gap_slices) {
            output.logical_input_causal_gap_duration_us = core::checked_add_u64(output.logical_input_causal_gap_duration_us,
                                                                                slice.owned_duration_us(),
                                                                                "HiCache logical-input-causal CPU gap duration exceeds uint64 range");
        }
        if (!output.logical_input_causal_gap_slices.empty()) {
            output.evidence.push_back(output.target_effective_token_count == 0 ? "target_omits_foreground_wait_logical_input_gap_projection"
                                                                               : "target_reduces_foreground_wait_by_payload_ratio");
        }
    }
    const auto owned_duration =
        core::checked_add_u64(operation.owned_node_duration_us, operation.owned_gap_duration_us, "HiCache attributed owned I/O duration exceeds uint64 range");
    output.residual_unknown_duration_us = operation.observed_duration_us > owned_duration ? operation.observed_duration_us - owned_duration : 0;
    output.start_anchor = operation.source_anchor_node_id;
    output.completion_anchor = operation.completion_anchor_node_id;
    if (output.consumer_anchors.empty() && operation.consumer_anchor_node_id) {
        output.consumer_anchors.push_back(*operation.consumer_anchor_node_id);
        output.consumer_anchor_method = "io_operation_ledger_consumer_boundary";
    }
}

} // namespace markov::trace_graph::modules::hicache::patch::attribution_detail
