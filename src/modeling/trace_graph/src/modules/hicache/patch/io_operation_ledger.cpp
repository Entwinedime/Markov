/**
 * @file
 * @brief Source-observed HiCache I/O operation ledger construction.
 */
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"

#include "io_operation_ledger_detail.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch {

namespace io_operation_ledger_detail {

std::optional<HiCacheIoOperationKind> operation_kind(std::string_view role) {
    if (role == "prefetch_io_observed") return HiCacheIoOperationKind::Prefetch;
    if (role == "loadback_io_observed") return HiCacheIoOperationKind::Load;
    if (role == "commit_device_to_host_io_observed") return HiCacheIoOperationKind::WriteDeviceToHost;
    if (role == "writeback_io_observed") return HiCacheIoOperationKind::WriteHostToStorage;
    return std::nullopt;
}

std::string direction(HiCacheIoOperationKind kind) {
    switch (kind) {
    case HiCacheIoOperationKind::Prefetch:
        return "storage_to_host";
    case HiCacheIoOperationKind::Load:
        return "host_to_device";
    case HiCacheIoOperationKind::WriteDeviceToHost:
        return "device_to_host";
    case HiCacheIoOperationKind::WriteHostToStorage:
        return "host_to_storage";
    }
    return "unknown";
}

std::string_view device_trace_direction(HiCacheIoOperationKind kind) {
    switch (kind) {
    case HiCacheIoOperationKind::Load:
        return "host to device";
    case HiCacheIoOperationKind::WriteDeviceToHost:
        return "device to host";
    case HiCacheIoOperationKind::Prefetch:
    case HiCacheIoOperationKind::WriteHostToStorage:
        return {};
    }
    return {};
}

std::string observed_span_semantics(HiCacheIoOperationKind kind) {
    switch (kind) {
    case HiCacheIoOperationKind::Prefetch:
        return "storage_to_host_transfer";
    case HiCacheIoOperationKind::Load:
        return "host_submission";
    case HiCacheIoOperationKind::WriteDeviceToHost:
        return "host_submission";
    case HiCacheIoOperationKind::WriteHostToStorage:
        return "asynchronous_transfer_observation";
    }
    return "unknown";
}

uint64_t fact_end(const HiCacheSourceFactNode & fact) {
    return fact.timestamp_us > std::numeric_limits<uint64_t>::max() - fact.duration_us ? std::numeric_limits<uint64_t>::max()
                                                                                       : fact.timestamp_us + fact.duration_us;
}

uint64_t fact_boundary(const HiCacheSourceFactNode & fact) { return fact.phase == "end" ? fact_end(fact) : fact.timestamp_us; }

bool contains_object_node(const HiCacheSourceFactNode & fact, uint64_t object_node_id) {
    return std::ranges::find(fact.operation_node_ids, object_node_id) != fact.operation_node_ids.end();
}

const HiCacheSourceFactNode * paired_call_start(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & end) {
    const HiCacheSourceFactNode * match = nullptr;
    const auto consider = [&](const HiCacheSourceFactNode & candidate) {
        if (candidate.phase != "start" || candidate.target_id != end.target_id || candidate.pid != end.pid || candidate.tid != end.tid
            || candidate.timestamp_us != end.timestamp_us)
            return true;
        if (match != nullptr) return false;
        match = &candidate;
        return true;
    };
    for (size_t node_id : source.nodes_for_fact_role(end.fact_role)) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate != nullptr && !consider(*candidate)) return nullptr;
    }
    for (const auto & candidate : source.tail_context_facts()) {
        if (!consider(candidate)) return nullptr;
    }
    return match;
}

uint64_t page_size_for_scope(const HiCacheSourceDagIndex & source, std::string_view cache_scope) {
    uint64_t page_size = 0;
    for (const auto & fact : source.fact_nodes()) {
        if (fact.cache_scope != cache_scope || fact.source_page_size == 0) continue;
        if (page_size != 0 && page_size != fact.source_page_size) return 0;
        page_size = fact.source_page_size;
    }
    for (const auto & fact : source.tail_context_facts()) {
        if (fact.cache_scope != cache_scope || fact.source_page_size == 0) continue;
        if (page_size != 0 && page_size != fact.source_page_size) return 0;
        page_size = fact.source_page_size;
    }
    return page_size;
}

uint64_t unique_source_page_size(const HiCacheSourceDagIndex & source) {
    uint64_t page_size = 0;
    const auto consider = [&](const HiCacheSourceFactNode & fact) {
        if (fact.source_page_size == 0) return true;
        if (page_size != 0 && page_size != fact.source_page_size) return false;
        page_size = fact.source_page_size;
        return true;
    };
    for (const auto & fact : source.fact_nodes()) {
        if (!consider(fact)) return 0;
    }
    for (const auto & fact : source.tail_context_facts()) {
        if (!consider(fact)) return 0;
    }
    return page_size;
}

const HiCacheSourceFactNode * load_decision_for_timing(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & timing) {
    const HiCacheSourceFactNode * latest = nullptr;
    for (size_t node_id : source.nodes_for_fact_role("loadback_decision_observed")) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate == nullptr || candidate->pid != timing.pid || !candidate->object_node_id || !contains_object_node(timing, *candidate->object_node_id)
            || candidate->timestamp_us > timing.timestamp_us)
            continue;
        if (latest == nullptr || candidate->timestamp_us > latest->timestamp_us) latest = candidate;
    }
    return latest;
}

const HiCacheSourceFactNode * request_consumer(const HiCacheSourceDagIndex & source, std::string_view request_id, std::string_view pid,
                                               uint64_t source_end_us) {
    const HiCacheSourceFactNode * earliest = nullptr;
    for (size_t node_id : source.nodes_for_request(request_id)) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate == nullptr || candidate->pid != pid
            || (candidate->fact_role != "request_admission_observed" && candidate->fact_role != "cache_extend_input")
            || fact_boundary(*candidate) < source_end_us)
            continue;
        if (earliest == nullptr || fact_boundary(*candidate) < fact_boundary(*earliest)) earliest = candidate;
    }
    return earliest;
}

const HiCacheSourceFactNode * request_role_before(const HiCacheSourceDagIndex & source, std::string_view request_id, std::string_view pid,
                                                  std::string_view role, uint64_t timestamp_us) {
    const HiCacheSourceFactNode * latest = nullptr;
    for (size_t node_id : source.nodes_for_request(request_id)) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate == nullptr || candidate->pid != pid || candidate->fact_role != role || fact_boundary(*candidate) > timestamp_us) continue;
        if (latest == nullptr || fact_boundary(*candidate) > fact_boundary(*latest)) latest = candidate;
    }
    return latest;
}

const HiCacheSourceFactNode * write_consumer(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & timing) {
    const HiCacheSourceFactNode * earliest = nullptr;
    const auto consider = [&](const HiCacheSourceFactNode & candidate) {
        if (candidate.fact_role != "commit_capacity_release_observed" || candidate.phase != "end" || candidate.pid != timing.pid
            || candidate.timestamp_us < fact_end(timing))
            return;
        bool matches = !timing.operation_id.empty() && candidate.operation_id == timing.operation_id;
        if (!matches) {
            matches = std::ranges::any_of(timing.operation_node_ids, [&](uint64_t object_node_id) { return contains_object_node(candidate, object_node_id); });
        }
        if (!matches) return;
        if (earliest == nullptr || candidate.timestamp_us < earliest->timestamp_us) earliest = &candidate;
    };
    for (size_t node_id : source.nodes_for_fact_role("commit_capacity_release_observed")) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate != nullptr) consider(*candidate);
    }
    for (const auto & candidate : source.tail_context_facts()) consider(candidate);
    return earliest;
}

uint64_t completed_tokens(const HiCacheSourceFactNode & fact) {
    if (fact.completed_token_count_present) return fact.completed_token_count;
    if (fact.effective_token_count > 0) return fact.effective_token_count;
    return fact.token_count;
}

void append_reason(HiCacheIoOperationRecord & record, std::string reason) {
    if (reason.empty()) return;
    if (!record.reason.empty()) record.reason += "; ";
    record.reason += std::move(reason);
}

bool fact_less(const HiCacheSourceFactNode * left, const HiCacheSourceFactNode * right) {
    if (left->timestamp_us != right->timestamp_us) return left->timestamp_us < right->timestamp_us;
    if (left->event_index != right->event_index) return left->event_index < right->event_index;
    return left->node_id < right->node_id;
}

void sort_unique(std::vector<size_t> & values) {
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

uint64_t gap_duration(std::span<const HiCacheCpuGapSlice> slices) {
    uint64_t total = 0;
    for (const auto & slice : slices) {
        total = core::checked_add_u64(total, slice.owned_duration_us(), "HiCache completion-wait gap duration exceeds uint64 range");
    }
    return total;
}

void build_prefetch_completion_wait_contract(const HiCacheSourceDagIndex & source, HiCacheIoOperationRecord & record) {
    record.completion_wait_status = "unresolved";
    record.source_completion_us = record.source_end_us;
    if (record.request_id.empty()) {
        record.completion_wait_reason = "prefetch completion wait has no exact request identity";
        return;
    }

    std::vector<const HiCacheSourceFactNode *> false_checks;
    std::vector<const HiCacheSourceFactNode *> true_checks;
    for (size_t node_id : source.nodes_for_request(record.request_id)) {
        const auto * fact = source.fact_node(node_id);
        if (fact == nullptr || fact->pid != record.pid || fact->fact_role != "prefetch_progress_observed" || fact->phase != "end" || !fact->progress_ready)
            continue;
        (*fact->progress_ready ? true_checks : false_checks).push_back(fact);
    }
    std::ranges::sort(false_checks, fact_less);
    std::ranges::sort(true_checks, fact_less);

    const HiCacheSourceFactNode * first_false = false_checks.empty() ? nullptr : false_checks.front();
    const HiCacheSourceFactNode * first_true = nullptr;
    if (first_false != nullptr) {
        const auto match = std::ranges::find_if(true_checks, [&](const auto * fact) { return fact->timestamp_us >= first_false->timestamp_us; });
        if (match != true_checks.end()) first_true = *match;
    }
    else if (!true_checks.empty()) first_true = true_checks.front();

    if (first_true == nullptr) {
        record.completion_wait_status = "ambiguous";
        record.completion_wait_reason = "prefetch progress observations do not provide one terminal ready check";
        return;
    }

    record.control_ready_us = first_false == nullptr ? first_true->timestamp_us : first_false->timestamp_us;
    record.wait_exit_start_us = first_true->timestamp_us;
    record.wait_exit_end_us = fact_end(*first_true);
    record.retained_terminal_control_us = first_true->duration_us;
    const auto & control_fact = first_false == nullptr ? *first_true : *first_false;
    record.control_ready_anchor_node_id = source.cpu_boundary_at_or_after(control_fact.pid, control_fact.tid, control_fact.timestamp_us);
    record.wait_exit_anchor_node_id = source.cpu_boundary_at_or_after(first_true->pid, first_true->tid, first_true->timestamp_us);
    record.terminal_control_anchor_node_id = record.wait_exit_anchor_node_id;

    if (first_false == nullptr) {
        if (!record.control_ready_anchor_node_id || !record.wait_exit_anchor_node_id) {
            record.completion_wait_status = "ambiguous";
            record.completion_wait_reason = "immediate-ready prefetch progress has no executable control boundary";
            return;
        }
        record.completion_wait_status = "ready";
        record.completion_join_contract_ready = true;
        record.completion_wait_reason = "the first progress check is immediately ready and supplies the target-side control branch";
        record.evidence.push_back("prefetch_immediate_ready_control_boundary");
        return;
    }

    for (const auto * check : false_checks) {
        if (check->timestamp_us > first_true->timestamp_us) break;
        auto ownership = source.timing_interval_ownership(*check);
        record.completion_wait_owned_node_ids.insert(record.completion_wait_owned_node_ids.end(),
                                                     ownership.owned_node_ids.begin(),
                                                     ownership.owned_node_ids.end());
    }
    sort_unique(record.completion_wait_owned_node_ids);

    const auto wait_window_duration = first_true->timestamp_us - first_false->timestamp_us;
    const auto wait_ownership = source.timing_interval_ownership(first_false->pid, first_false->tid, first_false->timestamp_us, wait_window_duration);
    record.completion_wait_slices = wait_ownership.owned_gap_slices;
    record.completion_wait_gap_duration_us = gap_duration(record.completion_wait_slices);
    record.logical_input_completion_wait_slices = source.project_foreground_gap_across_logical_input_lanes(record.completion_wait_slices);
    record.logical_input_completion_wait_duration_us = gap_duration(record.logical_input_completion_wait_slices);
    record.completion_wait_duration_us = record.source_completion_us > record.control_ready_us ? record.source_completion_us - record.control_ready_us : 0;
    const auto completion_or_control = std::max(record.control_ready_us, record.source_completion_us);
    record.polling_lag_us = record.wait_exit_start_us > completion_or_control ? record.wait_exit_start_us - completion_or_control : 0;
    record.source_completion_wait_blocking = true;

    const bool valid_order = record.control_ready_us <= record.source_completion_us && record.source_completion_us <= record.wait_exit_end_us;
    const bool anchors_ready = record.control_ready_anchor_node_id && record.wait_exit_anchor_node_id && record.terminal_control_anchor_node_id;
    if (!valid_order || !anchors_ready) {
        record.completion_wait_status = "ambiguous";
        record.completion_wait_reason = !valid_order ? "source completion is not enclosed by the false-to-true progress interval"
                                                     : "blocking prefetch progress has no complete executable boundary pair";
        return;
    }
    record.completion_wait_status = "ready";
    record.completion_join_contract_ready = true;
    record.completion_wait_reason = "first false progress is control-ready; storage completion and one retained terminal true check close the foreground wait";
    record.evidence.push_back("prefetch_false_to_completion_to_true_contract");
}

void build_loadback_readiness_contract(const HiCacheSourceDagIndex & source, HiCacheIoOperationRecord & record,
                                       const HiCacheDeviceTransferClosure & closure) {
    record.completion_wait_status = "unresolved";
    record.device_transfer_node_ids = closure.transfer_node_ids;
    record.device_completion_node_ids = closure.completion_node_ids;
    record.readiness_join_node_ids = closure.readiness_join_node_ids;
    record.device_transfer_duration_us = closure.transfer_duration_us;
    if (closure.status != "ready") {
        record.completion_wait_reason = closure.reason;
        return;
    }
    if (!record.consumer_anchor_node_id || *record.consumer_anchor_node_id >= source.graph().node_count()) {
        record.completion_wait_reason = "loadback device transfer has no exact request consumer boundary";
        return;
    }
    record.control_ready_anchor_node_id = record.consumer_anchor_node_id;
    record.wait_exit_anchor_node_id = record.consumer_anchor_node_id;
    record.terminal_control_anchor_node_id = record.consumer_anchor_node_id;
    const auto & consumer = source.graph().event_for_node(*record.consumer_anchor_node_id);
    record.control_ready_us = consumer.ts;
    record.wait_exit_start_us = consumer.ts;
    record.wait_exit_end_us = fact_end(HiCacheSourceFactNode{ .timestamp_us = consumer.ts, .duration_us = consumer.dur });
    record.retained_terminal_control_us = consumer.dur;
    record.source_completion_us = 0;
    for (size_t node_id : record.device_transfer_node_ids) {
        if (node_id >= source.graph().node_count()) continue;
        const auto & event = source.graph().event_for_node(node_id);
        record.source_completion_us = std::max(record.source_completion_us, fact_end(HiCacheSourceFactNode{ .timestamp_us = event.ts, .duration_us = event.dur }));
    }
    record.completion_wait_owned_node_ids = record.device_transfer_node_ids;
    record.completion_wait_duration_us = record.device_transfer_duration_us;
    record.source_completion_wait_blocking = true;
    record.completion_join_contract_ready = true;
    record.source_readiness_topology_ready = true;
    record.completion_wait_status = "ready_existing_device_event_join";
    record.completion_wait_reason =
        "host submission is preserved while direction-matched device transfers retain their existing event record/wait joins to the consumer";
    record.evidence.push_back("loadback_device_transfer_submit_closure");
    record.evidence.push_back("loadback_existing_layer_event_readiness_join");
}

void build_device_to_host_readiness_contract(const HiCacheSourceDagIndex & source, HiCacheIoOperationRecord & record,
                                             const HiCacheDeviceTransferClosure & closure) {
    record.completion_wait_status = "unresolved";
    record.device_transfer_node_ids = closure.transfer_node_ids;
    record.device_completion_node_ids = closure.completion_node_ids;
    record.readiness_join_node_ids = closure.readiness_join_node_ids;
    record.device_transfer_duration_us = closure.transfer_duration_us;
    record.source_completion_us = 0;
    for (size_t node_id : record.device_transfer_node_ids) {
        if (node_id >= source.graph().node_count()) continue;
        const auto & event = source.graph().event_for_node(node_id);
        record.source_completion_us = std::max(
            record.source_completion_us, fact_end(HiCacheSourceFactNode{ .timestamp_us = event.ts, .duration_us = event.dur }));
    }
    if (closure.status == "transfer_only") {
        record.completion_wait_owned_node_ids = record.device_transfer_node_ids;
        record.completion_wait_duration_us = record.device_transfer_duration_us;
        record.source_completion_wait_blocking = false;
        record.completion_wait_status = "ready_background_transfer_only";
        record.completion_wait_reason =
            "background device-to-host transfer is exact, but no retained completion/readiness topology is available for reuse";
        record.evidence.push_back("device_to_host_transfer_submit_closure");
        record.evidence.push_back("device_to_host_background_transfer_only");
        record.evidence.push_back("device_to_host_readiness_topology_not_reusable");
        record.evidence.push_back("device_to_host_host_submission_not_completion");
        return;
    }
    if (closure.status != "ready") {
        record.completion_wait_reason = closure.reason;
        return;
    }
    record.completion_wait_owned_node_ids = record.device_transfer_node_ids;
    record.completion_wait_duration_us = record.device_transfer_duration_us;
    record.source_completion_wait_blocking = false;
    record.completion_join_contract_ready = true;
    record.source_readiness_topology_ready = true;
    record.completion_wait_status = "ready_existing_device_event_join";
    record.completion_wait_reason =
        "host submission is preserved while direction-matched device-to-host transfers retain their existing completion topology";
    record.evidence.push_back("device_to_host_transfer_submit_closure");
    record.evidence.push_back("device_to_host_existing_event_readiness_join");
    record.evidence.push_back("device_to_host_host_submission_not_completion");
}

void build_load_admission_control(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & decision,
                                  HiCacheIoOperationRecord & record) {
    constexpr std::string_view control_event_name = "hicache.control.load_back_admission";
    const auto resolved = source.enclosing_control_interval_ownership(decision, control_event_name);
    if (!resolved) return;
    const auto & ownership = *resolved;
    record.admission_cpu_gap_slices = ownership.owned_gap_slices;
    for (const auto node_id : ownership.owned_node_ids) {
        const auto & event = source.graph().event_for_node(node_id);
        if (event.name.ends_with(".self")) {
            record.admission_python_self_node_ids.push_back(node_id);
            continue;
        }
        record.admission_explicit_node_ids.push_back(node_id);
    }
    if (ownership.status == "ready") {
        record.evidence.push_back("loadback_admission_control_interval");
        record.evidence.push_back("loadback_admission_explicit_children_only");
        record.evidence.push_back("loadback_admission_python_self_and_gap_are_nuisance");
    }
}

} // namespace io_operation_ledger_detail

bool hicache_io_operation_record_ready(const HiCacheIoOperationRecord & record) {
    return record.status == "ready" || record.status == "ready_background_unmaterialized"
           || record.status == "ready_background_transfer_only";
}

uint64_t HiCacheIoOperationLedger::ready_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(records, hicache_io_operation_record_ready));
}

uint64_t HiCacheIoOperationLedger::unresolved_count() const { return static_cast<uint64_t>(records.size()) - ready_count(); }

std::string hicache_io_operation_kind_name(HiCacheIoOperationKind kind) {
    switch (kind) {
    case HiCacheIoOperationKind::Prefetch:
        return "prefetch";
    case HiCacheIoOperationKind::Load:
        return "load";
    case HiCacheIoOperationKind::WriteDeviceToHost:
        return "write_device_to_host";
    case HiCacheIoOperationKind::WriteHostToStorage:
        return "write_host_to_storage";
    }
    return "unknown";
}

HiCacheIoOperationLedger build_hicache_io_operation_ledger(const HiCacheSourceDagIndex & source) {
    HiCacheIoOperationLedger ledger;
    const auto append_fact = [&](const HiCacheSourceFactNode & fact, bool causal_tail) {
        const auto kind = io_operation_ledger_detail::operation_kind(fact.fact_role);
        if (!kind || fact.fact_class != "timing_observation" || fact.phase != "end" || fact.duration_us == 0) return;
        auto record = io_operation_ledger_detail::build_record(source, fact, *kind);
        if (causal_tail) record.evidence.push_back("causal_tail_context");
        (void)core::checked_increment_u64(ledger.counts_by_kind[hicache_io_operation_kind_name(*kind)], "HiCache I/O ledger kind count exceeds uint64 range");
        (void)core::checked_increment_u64(ledger.counts_by_status[record.status], "HiCache I/O ledger status count exceeds uint64 range");
        if (!hicache_io_operation_record_ready(record)) {
            (void)core::checked_increment_u64(ledger.unresolved_reasons[record.reason], "HiCache I/O ledger unresolved count exceeds uint64 range");
        }
        ledger.records.push_back(std::move(record));
    };
    for (const auto & fact : source.fact_nodes()) append_fact(fact, false);
    for (const auto & fact : source.tail_context_facts()) append_fact(fact, true);
    if (ledger.records.empty()) ledger.status = "no_observed_io";
    else if (ledger.unresolved_count() == 0) ledger.status = "ready";
    else if (ledger.ready_count() > 0) ledger.status = "partial";
    else ledger.status = "blocked";
    return ledger;
}

} // namespace markov::trace_graph::modules::hicache::patch
