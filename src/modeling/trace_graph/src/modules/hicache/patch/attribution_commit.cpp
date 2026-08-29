/**
 * @file
 * @brief Commit D2H/H2S exact-ledger attribution and lifecycle identity.
 */
#include "attribution_common.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch::attribution_detail {

using model::HiCacheEffectDecision;
using model::HiCacheSourceCarrierState;

bool role_contract_available(const HiCacheSourceDagIndex & source, std::string_view role) {
    return source.stats().dag_patch_contract_ready || !source.nodes_for_fact_role(role).empty();
}

namespace {

bool nested_call(const HiCacheSourceFactNode & child, const HiCacheSourceFactNode & parent) {
    return child.pid == parent.pid && child.tid == parent.tid && child.timestamp_us >= parent.timestamp_us && fact_end(child) <= fact_end(parent);
}

bool contains_object_node(const HiCacheSourceFactNode & fact, uint64_t node_id) {
    return std::ranges::find(fact.operation_node_ids, node_id) != fact.operation_node_ids.end();
}

template <typename Fn> void for_each_role_fact_including_tail(const HiCacheSourceDagIndex & source, std::string_view role, Fn fn) {
    for (size_t node_id : source.nodes_for_fact_role(role)) {
        const auto * fact = source.fact_node(node_id);
        if (fact != nullptr) fn(*fact);
    }
    for (const auto & fact : source.tail_context_facts()) {
        if (fact.fact_role == role) fn(fact);
    }
}

const HiCacheSourceFactNode * paired_call_end(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & start) {
    const HiCacheSourceFactNode * match = nullptr;
    bool ambiguous = false;
    for_each_role_fact_including_tail(source, start.fact_role, [&](const auto & candidate) {
        if (candidate.phase != "end" || candidate.target_id != start.target_id || candidate.pid != start.pid || candidate.tid != start.tid
            || candidate.timestamp_us != start.timestamp_us)
            return;
        if (match != nullptr) ambiguous = true;
        else match = &candidate;
    });
    if (ambiguous) return nullptr;
    return match;
}

std::vector<const HiCacheSourceFactNode *> nested_role_facts(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor,
                                                             std::string_view role) {
    std::vector<const HiCacheSourceFactNode *> matches;
    for_each_role_fact_including_tail(source, role, [&](const auto & fact) {
        if (nested_call(fact, anchor)) matches.push_back(&fact);
    });
    std::ranges::sort(matches, [](const auto * left, const auto * right) {
        if (left->timestamp_us != right->timestamp_us) return left->timestamp_us < right->timestamp_us;
        return left->node_id < right->node_id;
    });
    return matches;
}

bool complete_d2h_page_identity(const HiCacheSourceFactNode & enqueue) {
    if (enqueue.page_hashes.empty()) return false;
    const std::set<std::string_view> unique_hashes(enqueue.page_hashes.begin(), enqueue.page_hashes.end());
    return unique_hashes.size() == enqueue.page_hashes.size()
           && std::ranges::all_of(enqueue.page_hashes, [](const auto & page_hash) { return !page_hash.empty(); });
}

bool exact_capacity_release_closure(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & enqueue) {
    if (!enqueue.object_node_id) return false;
    size_t match_count = 0;
    for_each_role_fact_including_tail(source, "commit_capacity_release_observed", [&](const auto & start) {
        if (start.phase != "start" || start.pid != enqueue.pid || fact_precedes(start, enqueue) || !contains_object_node(start, *enqueue.object_node_id))
            return;
        const auto * end = paired_call_end(source, start);
        if (end == nullptr || end->duration_us == 0) return;
        ++match_count;
    });
    return match_count == 1;
}

const HiCacheSourceFactNode * delayed_write_back_owner(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & enqueue) {
    if (enqueue.write_back != true || enqueue.phase != "end" || enqueue.effective_token_count == 0 || !enqueue.object_node_id
        || !complete_d2h_page_identity(enqueue) || !exact_capacity_release_closure(source, enqueue))
        return nullptr;

    const HiCacheSourceFactNode * capacity_result = nullptr;
    bool ambiguous_capacity_result = false;
    for_each_role_fact_including_tail(source, "capacity_result_observed", [&](const auto & candidate) {
        if (candidate.phase != "end" || candidate.pid != enqueue.pid || candidate.cache_scope != enqueue.cache_scope || !nested_call(enqueue, candidate))
            return;
        if (capacity_result != nullptr) ambiguous_capacity_result = true;
        else capacity_result = &candidate;
    });
    if (ambiguous_capacity_result || capacity_result == nullptr) return nullptr;

    const HiCacheSourceFactNode * owner = nullptr;
    for_each_role_fact_including_tail(source, "cache_lifecycle_commit", [&](const auto & candidate) {
        if (candidate.phase != "end" || candidate.pid != enqueue.pid || candidate.cache_scope != enqueue.cache_scope
            || fact_boundary(candidate) > capacity_result->timestamp_us || nested_call(enqueue, candidate))
            return;
        if (owner == nullptr || fact_boundary(*owner) < fact_boundary(candidate)
            || (fact_boundary(*owner) == fact_boundary(candidate) && fact_precedes(*owner, candidate)))
            owner = &candidate;
    });
    return owner;
}

const HiCacheSourceFactNode * paired_tail_call_end(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & start) {
    const HiCacheSourceFactNode * match = nullptr;
    for (const auto & candidate : source.tail_context_facts()) {
        if (candidate.fact_role != start.fact_role || candidate.phase != "end" || candidate.target_id != start.target_id || candidate.pid != start.pid
            || candidate.tid != start.tid || candidate.timestamp_us != start.timestamp_us)
            continue;
        if (match != nullptr) return nullptr;
        match = &candidate;
    }
    return match;
}

} // namespace

std::vector<const HiCacheSourceFactNode *> commit_d2h_enqueues(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor) {
    auto matches = nested_role_facts(source, anchor, "commit_device_to_host_enqueue_observed");
    matches.erase(std::remove_if(matches.begin(), matches.end(), [](const auto * fact) { return fact->phase != "end"; }), matches.end());
    for_each_role_fact_including_tail(source, "commit_device_to_host_enqueue_observed", [&](const auto & enqueue) {
        if (enqueue.phase != "end" || nested_call(enqueue, anchor)) return;
        const auto * owner = delayed_write_back_owner(source, enqueue);
        if (owner != nullptr && owner->node_id == anchor.node_id) matches.push_back(&enqueue);
    });
    std::ranges::sort(matches, [](const auto * left, const auto * right) {
        if (left->timestamp_us != right->timestamp_us) return left->timestamp_us < right->timestamp_us;
        return left->node_id < right->node_id;
    });
    matches.erase(std::unique(matches.begin(), matches.end(), [](const auto * left, const auto * right) { return left->node_id == right->node_id; }),
                  matches.end());
    return matches;
}

std::vector<const HiCacheSourceFactNode *> capacity_release_calls(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor) {
    std::set<uint64_t> object_node_ids;
    for (const auto * enqueue : commit_d2h_enqueues(source, anchor)) {
        if (enqueue->effective_token_count > 0 && enqueue->object_node_id) object_node_ids.insert(*enqueue->object_node_id);
    }
    std::vector<const HiCacheSourceFactNode *> releases;
    for_each_role_fact_including_tail(source, "commit_capacity_release_observed", [&](const auto & start) {
        if (start.phase != "start" || start.pid != anchor.pid) return;
        const bool owns_node = std::ranges::any_of(object_node_ids, [&](uint64_t object_node_id) { return contains_object_node(start, object_node_id); });
        if (!owns_node) return;
        if (const auto * end = paired_call_end(source, start)) releases.push_back(end);
    });
    std::ranges::sort(releases, [](const auto * left, const auto * right) {
        if (left->timestamp_us != right->timestamp_us) return left->timestamp_us < right->timestamp_us;
        return left->node_id < right->node_id;
    });
    return releases;
}

bool all_capacity_releases_in_tail(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor) {
    std::set<uint64_t> expected;
    for (const auto * enqueue : commit_d2h_enqueues(source, anchor)) {
        if (enqueue->effective_token_count > 0 && enqueue->object_node_id) expected.insert(*enqueue->object_node_id);
    }
    if (expected.empty()) return false;

    std::set<uint64_t> closed;
    for (const auto & start : source.tail_context_facts()) {
        if (start.fact_role != "commit_capacity_release_observed" || start.phase != "start" || start.pid != anchor.pid) continue;
        const auto * end = paired_tail_call_end(source, start);
        if (end == nullptr || end->duration_us == 0) continue;
        for (uint64_t object_node_id : start.operation_node_ids) {
            if (expected.contains(object_node_id)) closed.insert(object_node_id);
        }
    }
    return closed == expected;
}

namespace {

std::vector<const HiCacheIoOperationRecord *> d2h_records_for_commit(const HiCacheSourceDagIndex & source, const HiCacheIoOperationLedger & operations,
                                                                     const HiCacheSourceFactNode & anchor) {
    std::set<uint64_t> object_node_ids;
    for (const auto * enqueue : commit_d2h_enqueues(source, anchor)) {
        if (enqueue->effective_token_count > 0 && enqueue->object_node_id) object_node_ids.insert(*enqueue->object_node_id);
    }
    std::vector<const HiCacheIoOperationRecord *> records;
    for (const auto & record : operations.records) {
        if (record.kind != HiCacheIoOperationKind::WriteDeviceToHost || record.pid != anchor.pid || !ledger_record_ready(record)) continue;
        const auto * timing = source.fact_node(record.timing_fact_node_id);
        if (timing == nullptr || record.operation_node_ids.empty()) continue;
        const bool exact_object_set =
            std::ranges::all_of(record.operation_node_ids, [&](uint64_t object_node_id) { return object_node_ids.contains(object_node_id); });
        if (!exact_object_set) continue;
        const bool call_identity = std::ranges::any_of(commit_d2h_enqueues(source, anchor), [&](const auto * enqueue) {
            return nested_call(*timing, *enqueue)
                   && std::ranges::any_of(record.operation_node_ids, [&](uint64_t object_node_id) { return enqueue->object_node_id == object_node_id; });
        });
        if (call_identity) records.push_back(&record);
    }
    std::ranges::sort(records, [](const auto * left, const auto * right) { return left->record_id < right->record_id; });
    records.erase(std::unique(records.begin(), records.end()), records.end());
    return records;
}

std::vector<const HiCacheIoOperationRecord *> h2s_records_for_commit(const HiCacheSourceDagIndex & source, const HiCacheIoOperationLedger & operations,
                                                                     const HiCacheSourceFactNode & anchor) {
    std::set<std::string> operation_ids;
    for (const auto * release : capacity_release_calls(source, anchor)) {
        for (const auto * enqueue : nested_role_facts(source, *release, "writeback_enqueue_observed")) {
            if (enqueue->phase == "end" && !enqueue->operation_id.empty()) operation_ids.insert(enqueue->operation_id);
        }
    }
    std::vector<const HiCacheIoOperationRecord *> records;
    for (const auto & record : operations.records) {
        if (record.kind != HiCacheIoOperationKind::WriteHostToStorage || record.pid != anchor.pid || !ledger_record_ready(record)) continue;
        if (operation_ids.contains(record.operation_id)) records.push_back(&record);
    }
    std::ranges::sort(records, [](const auto * left, const auto * right) { return left->record_id < right->record_id; });
    records.erase(std::unique(records.begin(), records.end()), records.end());
    return records;
}

} // namespace

void classify_commit_d2h_from_ledger(const HiCacheSourceDagIndex & source, const HiCacheIoOperationLedger & operations, const HiCacheEffectDecision & decision,
                                     const HiCacheSourceFactNode & anchor, HiCacheSourceAttribution & output) {
    const auto records = d2h_records_for_commit(source, operations, anchor);
    std::set<uint64_t> expected_object_node_ids;
    for (const auto * enqueue : commit_d2h_enqueues(source, anchor)) {
        if (enqueue->effective_token_count > 0 && enqueue->object_node_id) expected_object_node_ids.insert(*enqueue->object_node_id);
    }
    if (records.empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.reason = "source lifecycle has no exact D2H operation-ledger record";
        return;
    }
    std::map<uint64_t, size_t> record_counts_by_object_node;
    for (const auto * record : records) {
        for (uint64_t object_node_id : record->operation_node_ids) {
            if (expected_object_node_ids.contains(object_node_id)) ++record_counts_by_object_node[object_node_id];
        }
    }
    const bool exact_identity = std::ranges::all_of(expected_object_node_ids, [&](uint64_t object_node_id) {
        const auto found = record_counts_by_object_node.find(object_node_id);
        return found != record_counts_by_object_node.end() && found->second == 1;
    });
    if (!exact_identity) {
        output.source_carrier_state = HiCacheSourceCarrierState::Ambiguous;
        output.reason = "source lifecycle does not map each D2H tree node to exactly one operation-ledger record";
        return;
    }

    output.consumer_anchors.clear();
    output.consumer_anchor_method = "background_device_to_host_without_direct_foreground_consumer";
    output.source_completed_token_count = 0;
    output.target_effective_token_count = target_effective_token_count(decision);
    output.observed_io_duration_us = 0;
    output.residual_unknown_duration_us = 0;
    output.source_readiness_topology_ready = true;
    output.completion_join_contract_ready = true;
    output.completion_wait_status = "ready_existing_device_event_join";
    output.completion_wait_reason = "all lifecycle-local D2H operations retain exact source device-transfer completion topology";
    const auto enqueues = commit_d2h_enqueues(source, anchor);
    output.source_effect_schedule_aligned = std::ranges::all_of(enqueues, [&](const auto * enqueue) { return nested_call(*enqueue, anchor); });
    output.evidence.push_back(output.source_effect_schedule_aligned ? "d2h_lifecycle_local_schedule" : "d2h_delayed_write_back_schedule");
    if (!output.source_effect_schedule_aligned) {
        output.evidence.push_back("d2h_capacity_result_trigger_owner");
        output.evidence.push_back("d2h_victim_page_identity_complete");
    }
    bool source_transfer_set_ready = true;
    for (const auto * record : records) {
        output.io_operation_record_ids.push_back(record->record_id);
        output.timing_fact_nodes.push_back(record->timing_fact_node_id);
        output.control_fact_nodes.insert(output.control_fact_nodes.end(), record->control_fact_node_ids.begin(), record->control_fact_node_ids.end());
        output.operation_chain_nodes.insert(output.operation_chain_nodes.end(), record->control_fact_node_ids.begin(), record->control_fact_node_ids.end());
        output.owned_duration_nodes.insert(output.owned_duration_nodes.end(), record->device_transfer_node_ids.begin(), record->device_transfer_node_ids.end());
        output.completion_wait_owned_node_ids.insert(output.completion_wait_owned_node_ids.end(),
                                                     record->device_transfer_node_ids.begin(),
                                                     record->device_transfer_node_ids.end());
        output.source_completion_node_ids.insert(output.source_completion_node_ids.end(),
                                                 record->device_completion_node_ids.begin(),
                                                 record->device_completion_node_ids.end());
        output.readiness_join_node_ids.insert(output.readiness_join_node_ids.end(),
                                              record->readiness_join_node_ids.begin(),
                                              record->readiness_join_node_ids.end());
        output.source_completed_token_count = core::checked_add_u64(output.source_completed_token_count,
                                                                    record->completed_token_count,
                                                                    "HiCache attributed D2H token count exceeds uint64 range");
        output.observed_io_duration_us = core::checked_add_u64(output.observed_io_duration_us,
                                                               record->device_transfer_duration_us,
                                                               "HiCache attributed D2H transfer duration exceeds uint64 range");
        output.residual_unknown_duration_us = core::checked_add_u64(output.residual_unknown_duration_us,
                                                                    record->observed_duration_us,
                                                                    "HiCache attributed D2H host-submission duration exceeds uint64 range");
        append_snapshot_isolated_control_ownership(
            source,
            source.timing_interval_ownership(record->pid, record->tid, record->source_start_us, record->observed_duration_us),
            record->pid,
            record->tid,
            output);
        output.source_completion_us = std::max(output.source_completion_us, record->source_completion_us);
        source_transfer_set_ready = source_transfer_set_ready && !record->device_transfer_node_ids.empty();
        output.source_readiness_topology_ready = output.source_readiness_topology_ready && record->source_readiness_topology_ready;
        output.completion_join_contract_ready = output.completion_join_contract_ready && record->completion_join_contract_ready;
        output.evidence.insert(output.evidence.end(), record->evidence.begin(), record->evidence.end());
        output.evidence.push_back("d2h_operation_ledger_record:" + record->record_id);
    }
    sort_unique(output.timing_fact_nodes);
    sort_unique(output.control_fact_nodes);
    sort_unique(output.operation_chain_nodes);
    sort_unique(output.owned_duration_nodes);
    sort_unique(output.completion_wait_owned_node_ids);
    sort_unique(output.source_completion_node_ids);
    sort_unique(output.readiness_join_node_ids);
    std::ranges::sort(output.io_operation_record_ids);
    output.io_operation_record_ids.erase(std::unique(output.io_operation_record_ids.begin(), output.io_operation_record_ids.end()),
                                         output.io_operation_record_ids.end());
    finalize_source_control_ownership(source, output);
    output.source_control_removal_required = !output.source_control_duration_nodes.empty() || !output.source_control_gap_slices.empty();
    if (output.source_control_removal_required) {
        output.evidence.push_back("source_d2h_host_control_fully_owned");
        output.evidence.push_back("source_host_control_snapshot_excluded");
        output.evidence.push_back("source_host_control_logical_input_gap_projection");
    }
    if (!source_transfer_set_ready || output.owned_duration_nodes.empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.reason = "exact D2H operation set has no complete device-transfer closure";
        return;
    }
    if (!output.source_readiness_topology_ready) {
        output.completion_join_contract_ready = false;
        output.completion_wait_status = "ready_background_transfer_only";
        output.completion_wait_reason = "background D2H transfer nodes are exact, but incomplete source completion topology cannot be carried";
    }
    output.source_carrier_state = HiCacheSourceCarrierState::Present;
    output.observed_span_semantics = "host_submission";
    if (!output.source_readiness_topology_ready) {
        output.reason = "exact background D2H transfer set is source-owned, but its incomplete completion topology requires target-boundary reconstruction";
        output.evidence.push_back("d2h_source_transfer_rebuild_required");
    }
    else {
        output.reason = output.source_effect_schedule_aligned
                            ? "exact lifecycle-local D2H operation set owns the original device transfers and readiness topology"
                            : "exact delayed write-back D2H operation set is source-owned but must be rescheduled at the target lifecycle boundary";
        output.evidence.push_back("d2h_source_transfer_topology_reused");
    }
    output.evidence.push_back("d2h_tree_node_set_exact");
    (void)source;
}

void classify_commit_h2s_from_ledger(const HiCacheSourceDagIndex & source, const HiCacheIoOperationLedger & operations, const HiCacheSourceFactNode & anchor,
                                     HiCacheSourceAttribution & output) {
    const auto records = h2s_records_for_commit(source, operations, anchor);
    if (records.empty()) {
        if (all_capacity_releases_in_tail(source, anchor)) {
            output.source_carrier_state = HiCacheSourceCarrierState::Absent;
            output.evidence.push_back("post_window_commit_capacity_release_observed");
            output.reason = "source H2S completes after the measured window and has no formal consumer";
            return;
        }
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.reason = "source lifecycle has no exact H2S operation-ledger record";
        return;
    }
    for (const auto * record : records) {
        output.timing_fact_nodes.push_back(record->timing_fact_node_id);
        output.control_fact_nodes.insert(output.control_fact_nodes.end(), record->control_fact_node_ids.begin(), record->control_fact_node_ids.end());
        output.operation_chain_nodes.insert(output.operation_chain_nodes.end(), record->control_fact_node_ids.begin(), record->control_fact_node_ids.end());
        output.evidence.push_back("h2s_operation_ledger_record:" + record->record_id);
    }
    sort_unique(output.timing_fact_nodes);
    sort_unique(output.control_fact_nodes);
    sort_unique(output.operation_chain_nodes);
    output.source_carrier_state = HiCacheSourceCarrierState::Absent;
    output.reason = "exact H2S operation set is background-only in the source CPU DAG; target materialization is causal only through a target capacity gate";
}

} // namespace markov::trace_graph::modules::hicache::patch::attribution_detail
