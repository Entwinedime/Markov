/**
 * @file
 * @brief Materializes one source-observed HiCache I/O operation record.
 */
#include "io_operation_ledger_detail.hpp"

#include <algorithm>
#include <ranges>

namespace markov::trace_graph::modules::hicache::patch::io_operation_ledger_detail {

HiCacheIoOperationRecord build_record(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & timing, HiCacheIoOperationKind kind) {
    auto timing_view = timing;
    const auto * call_start = paired_call_start(source, timing);
    if (call_start != nullptr) {
        if (timing_view.operation_node_ids.empty()) timing_view.operation_node_ids = call_start->operation_node_ids;
        if (timing_view.token_count == 0) timing_view.token_count = call_start->token_count;
        if (timing_view.cache_scope.empty()) timing_view.cache_scope = call_start->cache_scope;
    }
    const auto scope_page_size = page_size_for_scope(source, timing_view.cache_scope);
    const auto trace_page_size = scope_page_size > 0 ? scope_page_size : unique_source_page_size(source);
    auto ownership = source.timing_interval_ownership(timing_view);
    HiCacheIoOperationRecord record{
        .record_id = timing.fact_role + ":" + timing.pid + ":" + std::to_string(timing.event_index),
        .kind = kind,
        .direction = direction(kind),
        .request_id = timing.request_id,
        .operation_id = timing.operation_id,
        .operation_node_ids = timing_view.operation_node_ids,
        .cache_scope = timing.cache_scope,
        .pid = timing.pid,
        .tid = timing.tid,
        .effective_token_count = timing_view.effective_token_count,
        .completed_token_count = completed_tokens(timing_view),
        .completed_token_count_present = timing_view.completed_token_count_present,
        .source_page_size = timing_view.source_page_size > 0 ? timing_view.source_page_size : trace_page_size,
        .full_path_span = timing.full_path_span,
        .source_start_us = timing.timestamp_us,
        .source_end_us = fact_end(timing),
        .observed_duration_us = timing.duration_us,
        .observed_span_semantics = observed_span_semantics(kind),
        .owned_node_duration_us = ownership.owned_node_duration_us,
        .owned_gap_duration_us = ownership.owned_gap_duration_us,
        .overlapping_node_duration_us = ownership.overlapping_node_duration_us,
        .max_node_overlap_us = ownership.max_node_overlap_us,
        .uncovered_duration_us = ownership.uncovered_duration_us,
        .timing_fact_node_id = timing.node_id,
        .runtime_node_ids = ownership.owned_node_ids,
        .cpu_gap_slices = ownership.owned_gap_slices,
        .cpu_overlap_slices = ownership.overlapping_node_slices,
        .source_anchor_node_id = ownership.start_anchor_node_id,
        .completion_anchor_node_id = ownership.completion_anchor_node_id,
        .runtime_copy_observed = !ownership.owned_node_ids.empty(),
        .foreground_consumer_required = kind == HiCacheIoOperationKind::Prefetch || kind == HiCacheIoOperationKind::Load,
        .evidence = { timing.fact_role, "same_pid_tid_call_containment" },
    };
    if (timing_view.source_page_size == 0 && scope_page_size == 0 && trace_page_size > 0)
        record.evidence.push_back("unique_source_trace_page_size_fallback");
    if (call_start != nullptr) record.control_fact_node_ids.push_back(call_start->node_id);
    const auto * load_decision = kind == HiCacheIoOperationKind::Load ? load_decision_for_timing(source, timing_view) : nullptr;
    if (kind == HiCacheIoOperationKind::Load && record.request_id.empty() && load_decision != nullptr) {
        record.request_id = load_decision->request_id;
        record.control_fact_node_ids.push_back(load_decision->node_id);
        record.evidence.push_back("tree_node_identity");
        if (record.effective_token_count == 0) record.effective_token_count = load_decision->effective_token_count;
    }
    if (kind == HiCacheIoOperationKind::Load && record.completed_token_count > 0 && load_decision != nullptr)
        build_load_admission_control(source, *load_decision, record);

    if (kind == HiCacheIoOperationKind::Prefetch && !record.request_id.empty()) {
        if (const auto * candidate = request_role_before(source, record.request_id, record.pid, "prefetch_candidate_anchor", record.source_start_us)) {
            const auto foreground = source.timing_interval_ownership(candidate->pid, candidate->tid, record.source_start_us, record.observed_duration_us);
            if (!foreground.owned_gap_slices.empty() && foreground.start_anchor_node_id && foreground.completion_anchor_node_id) {
                ownership = foreground;
                record.runtime_node_ids.clear();
                record.cpu_gap_slices = foreground.owned_gap_slices;
                record.owned_node_duration_us = 0;
                record.owned_gap_duration_us = foreground.owned_gap_duration_us;
                record.overlapping_node_duration_us = 0;
                record.max_node_overlap_us = 0;
                record.uncovered_duration_us =
                    record.observed_duration_us > record.owned_gap_duration_us ? record.observed_duration_us - record.owned_gap_duration_us : 0;
                record.source_anchor_node_id = foreground.start_anchor_node_id;
                record.completion_anchor_node_id = foreground.completion_anchor_node_id;
                record.cpu_overlap_slices.clear();
                record.runtime_copy_observed = false;
                record.control_fact_node_ids.push_back(candidate->node_id);
                record.evidence.push_back("wait_complete_foreground_gap_projection");
            }
        }
    }

    const HiCacheSourceFactNode * consumer = nullptr;
    if (!record.request_id.empty()) consumer = request_consumer(source, record.request_id, record.pid, record.source_end_us);
    else if (kind == HiCacheIoOperationKind::WriteDeviceToHost || kind == HiCacheIoOperationKind::WriteHostToStorage)
        consumer = write_consumer(source, timing);
    if (consumer != nullptr) {
        record.consumer_anchor_node_id = source.cpu_boundary_at_or_after(consumer->pid, consumer->tid, fact_boundary(*consumer));
        record.control_fact_node_ids.push_back(consumer->node_id);
        record.evidence.push_back("consumer_same_pid_tid_boundary");
    }

    if (kind == HiCacheIoOperationKind::Prefetch) {
        build_prefetch_completion_wait_contract(source, record);
        if (!record.consumer_anchor_node_id && record.completion_join_contract_ready && record.terminal_control_anchor_node_id
            && *record.terminal_control_anchor_node_id < source.graph().node_count()) {
            record.consumer_anchor_node_id = record.terminal_control_anchor_node_id;
            record.evidence.push_back("prefetch_terminal_ready_consumer_boundary");
        }
    }
    else if (kind == HiCacheIoOperationKind::Load) {
        const auto closure = source.device_transfer_closure(timing_view, device_trace_direction(kind));
        build_loadback_readiness_contract(source, record, closure);
        record.evidence.push_back("loadback_host_submission_not_completion");
    }
    else if (kind == HiCacheIoOperationKind::WriteDeviceToHost) {
        const auto closure = source.device_transfer_closure(timing_view, device_trace_direction(kind));
        build_device_to_host_readiness_contract(source, record, closure);
    }

    if (record.completed_token_count == 0) {
        const bool zero_payload_foreground = kind == HiCacheIoOperationKind::Prefetch || kind == HiCacheIoOperationKind::Load;
        const bool existing_zero_payload_control_contract = record.completion_join_contract_ready && record.control_ready_anchor_node_id
                                                            && record.wait_exit_anchor_node_id && record.terminal_control_anchor_node_id;
        record.runtime_node_ids.clear();
        record.cpu_gap_slices.clear();
        record.cpu_overlap_slices.clear();
        record.owned_node_duration_us = 0;
        record.owned_gap_duration_us = 0;
        record.overlapping_node_duration_us = 0;
        record.max_node_overlap_us = 0;
        record.uncovered_duration_us = record.observed_duration_us;
        record.source_anchor_node_id = std::nullopt;
        record.completion_anchor_node_id = std::nullopt;
        if (!zero_payload_foreground) record.consumer_anchor_node_id = std::nullopt;
        record.device_transfer_node_ids.clear();
        record.device_completion_node_ids.clear();
        record.readiness_join_node_ids.clear();
        record.device_transfer_duration_us = 0;
        record.source_readiness_topology_ready = false;
        record.completion_wait_owned_node_ids.clear();
        record.runtime_copy_observed = false;
        record.foreground_consumer_required = zero_payload_foreground;
        record.completion_join_contract_ready = existing_zero_payload_control_contract;
        if (!existing_zero_payload_control_contract && zero_payload_foreground && record.consumer_anchor_node_id
            && *record.consumer_anchor_node_id < source.graph().node_count()) {
            const auto & consumer_event = source.graph().event_for_node(*record.consumer_anchor_node_id);
            record.control_ready_us = consumer_event.ts;
            record.wait_exit_start_us = consumer_event.ts;
            record.wait_exit_end_us = fact_end(HiCacheSourceFactNode{ .timestamp_us = consumer_event.ts, .duration_us = consumer_event.dur });
            record.retained_terminal_control_us = consumer_event.dur;
            record.control_ready_anchor_node_id = record.consumer_anchor_node_id;
            record.wait_exit_anchor_node_id = record.consumer_anchor_node_id;
            record.terminal_control_anchor_node_id = record.consumer_anchor_node_id;
            record.completion_join_contract_ready = true;
            record.completion_wait_status = "ready_zero_payload_control_boundary";
            record.completion_wait_reason = "zero-payload source I/O reaches its canonical consumer through the immediate-ready control branch";
            record.evidence.push_back("zero_payload_foreground_control_boundary");
        }
        record.evidence.push_back(record.completed_token_count_present ? "explicit_zero_completed_payload" : "inferred_zero_effective_payload");
        record.status = "ready";
        record.reason = "zero completed payload requires no timing ownership or foreground consumer; probe wall span remains residual";
        return record;
    }

    const auto clear_host_ownership = [&] {
        record.runtime_node_ids.clear();
        record.cpu_gap_slices.clear();
        record.cpu_overlap_slices.clear();
        record.owned_node_duration_us = 0;
        record.owned_gap_duration_us = 0;
        record.overlapping_node_duration_us = 0;
        record.max_node_overlap_us = 0;
        record.uncovered_duration_us = record.observed_duration_us;
        record.source_anchor_node_id = std::nullopt;
        record.completion_anchor_node_id = std::nullopt;
        record.runtime_copy_observed = false;
    };
    if (kind == HiCacheIoOperationKind::WriteHostToStorage && ownership.status != "ready") {
        if (record.operation_id.empty()) {
            append_reason(record, "background H2S timing observation has no operation identity");
            return record;
        }
        clear_host_ownership();
        record.foreground_consumer_required = false;
        record.evidence.push_back("background_h2s_without_executable_cpu_lane");
        record.status = "ready_background_unmaterialized";
        record.reason = "background H2S has exact operation identity but no retained executable CPU lane; it remains an asynchronous ledger artifact until a "
                        "target capacity gate makes it causal";
        return record;
    }
    if (kind == HiCacheIoOperationKind::WriteDeviceToHost && record.source_readiness_topology_ready) {
        clear_host_ownership();
        record.evidence.push_back("device_to_host_host_submission_cpu_ownership_not_required");
        record.status = "ready";
        record.reason = "D2H identity, payload, device transfer, completion, and readiness joins are complete; host submission CPU self-time remains residual";
        return record;
    }
    if (kind == HiCacheIoOperationKind::WriteDeviceToHost && record.completion_wait_status == "ready_background_transfer_only"
        && !record.device_transfer_node_ids.empty()) {
        clear_host_ownership();
        record.evidence.push_back("device_to_host_host_submission_cpu_ownership_not_required");
        record.status = "ready_background_transfer_only";
        record.reason = "background D2H identity, payload, and device transfers are exact; incomplete completion topology must not be carried";
        return record;
    }

    const bool foreground_gap_projection = std::ranges::find(record.evidence, "wait_complete_foreground_gap_projection") != record.evidence.end();
    if (ownership.status != "ready" && !foreground_gap_projection) append_reason(record, ownership.reason);
    if (foreground_gap_projection && record.owned_gap_duration_us == 0) append_reason(record, "blocking prefetch has no owned foreground CPU gap");
    if (record.foreground_consumer_required && record.request_id.empty()) append_reason(record, "foreground I/O operation has no exact request identity");
    if (record.foreground_consumer_required && !record.consumer_anchor_node_id)
        append_reason(record, "foreground I/O operation has no executable consumer boundary");
    if (kind == HiCacheIoOperationKind::Load && !record.source_readiness_topology_ready)
        append_reason(record, "loadback operation has no proven device-transfer/event-readiness closure: " + record.completion_wait_reason);
    if (kind == HiCacheIoOperationKind::WriteDeviceToHost && !record.source_readiness_topology_ready)
        append_reason(record, "device-to-host operation has no proven device-transfer/event-readiness closure: " + record.completion_wait_reason);
    if (record.completed_token_count > 0 && record.source_page_size == 0) append_reason(record, "nonzero payload has no consistent source page size");
    if (record.reason.empty()) {
        record.status = "ready";
        record.reason = "operation identity, payload, same-thread timing ownership, and required consumer are complete";
    }
    return record;
}

} // namespace markov::trace_graph::modules::hicache::patch::io_operation_ledger_detail
