/**
 * @file
 * @brief Shared exact-ledger attribution helper implementation.
 */
#include "attribution_common.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::modules::hicache::patch::attribution_detail {

bool ledger_record_ready(const HiCacheIoOperationRecord & record) { return hicache_io_operation_record_ready(record); }

void sort_unique(std::vector<size_t> & values) {
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

uint64_t fact_end(const HiCacheSourceFactNode & fact) {
    if (fact.timestamp_us > std::numeric_limits<uint64_t>::max() - fact.duration_us) return std::numeric_limits<uint64_t>::max();
    return fact.timestamp_us + fact.duration_us;
}

uint64_t fact_boundary(const HiCacheSourceFactNode & fact) { return fact.phase == "end" ? fact_end(fact) : fact.timestamp_us; }

bool fact_precedes(const HiCacheSourceFactNode & left, const HiCacheSourceFactNode & right) {
    if (left.timestamp_us != right.timestamp_us) return left.timestamp_us < right.timestamp_us;
    if (left.event_index != right.event_index) return left.event_index < right.event_index;
    return left.node_id < right.node_id;
}

uint64_t target_effective_token_count(const model::HiCacheEffectDecision & decision) {
    uint64_t total = 0;
    for (const auto & segment : decision.effective_segments) {
        if (segment.token_end < segment.token_begin) throw std::logic_error("HiCache target effect segment has an inverted token interval");
        total = core::checked_add_u64(total, segment.token_end - segment.token_begin, "HiCache target effective token count exceeds uint64 range");
    }
    return total;
}

namespace {

using TimeInterval = std::pair<uint64_t, uint64_t>;

std::vector<TimeInterval> snapshot_intervals(const HiCacheSourceDagIndex & source, std::string_view pid, std::string_view tid, uint64_t start_us,
                                             uint64_t end_us) {
    std::vector<TimeInterval> intervals;
    for (size_t node_id : source.nodes_for_fact_role("state_snapshot")) {
        const auto * fact = source.fact_node(node_id);
        if (fact == nullptr || fact->pid != pid || fact->tid != tid || fact->duration_us == 0) continue;
        const auto overlap_start = std::max(start_us, fact->timestamp_us);
        const auto overlap_end = std::min(end_us, fact_end(*fact));
        if (overlap_end > overlap_start) intervals.emplace_back(overlap_start, overlap_end);
    }
    std::ranges::sort(intervals);
    std::vector<TimeInterval> merged;
    for (const auto & interval : intervals) {
        if (!merged.empty() && interval.first <= merged.back().second) merged.back().second = std::max(merged.back().second, interval.second);
        else merged.push_back(interval);
    }
    return merged;
}

bool overlaps_any(uint64_t start_us, uint64_t end_us, std::span<const TimeInterval> intervals) {
    return std::ranges::any_of(intervals, [&](const auto & interval) { return std::max(start_us, interval.first) < std::min(end_us, interval.second); });
}

} // namespace

void append_snapshot_isolated_control_ownership(const HiCacheSourceDagIndex & source, const HiCacheTimingIntervalOwnership & ownership, std::string_view pid,
                                                std::string_view tid, HiCacheSourceAttribution & output) {
    if (ownership.status != "ready") return;
    const auto excluded = snapshot_intervals(source, pid, tid, ownership.interval_start_us, ownership.interval_end_us);
    for (size_t node_id : ownership.owned_node_ids) {
        const auto & event = source.graph().event_for_node(node_id);
        const auto event_end = event.ts > std::numeric_limits<uint64_t>::max() - event.dur ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
        if (!overlaps_any(event.ts, event_end, excluded)) output.source_control_duration_nodes.push_back(node_id);
    }
    for (const auto & gap : ownership.owned_gap_slices) {
        uint64_t cursor = gap.owned_start_us;
        for (const auto & interval : excluded) {
            if (interval.second <= cursor) continue;
            if (interval.first >= gap.owned_end_us) break;
            if (interval.first > cursor) {
                auto retained = gap;
                retained.owned_start_us = cursor;
                retained.owned_end_us = std::min(interval.first, gap.owned_end_us);
                if (retained.owned_end_us > retained.owned_start_us) output.source_control_gap_slices.push_back(retained);
            }
            cursor = std::max(cursor, interval.second);
            if (cursor >= gap.owned_end_us) break;
        }
        if (cursor < gap.owned_end_us) {
            auto retained = gap;
            retained.owned_start_us = cursor;
            retained.owned_end_us = gap.owned_end_us;
            output.source_control_gap_slices.push_back(retained);
        }
    }
}

void finalize_source_control_ownership(const HiCacheSourceDagIndex & source, HiCacheSourceAttribution & output) {
    sort_unique(output.source_control_duration_nodes);
    for (size_t node_id : output.source_control_duration_nodes) {
        const auto source_duration = source.graph().node(node_id).duration;
        output.source_control_duration_us =
            core::checked_add_u64(output.source_control_duration_us, source_duration, "HiCache source CPU-control duration exceeds uint64 range");
    }
    const auto merge_gaps = [](std::vector<HiCacheCpuGapSlice> gaps) {
        std::ranges::sort(gaps, [](const auto & left, const auto & right) {
            if (left.owner_node_id != right.owner_node_id) return left.owner_node_id < right.owner_node_id;
            if (left.owned_start_us != right.owned_start_us) return left.owned_start_us < right.owned_start_us;
            return left.owned_end_us < right.owned_end_us;
        });
        std::vector<HiCacheCpuGapSlice> merged;
        for (const auto & gap : gaps) {
            if (!merged.empty() && merged.back().owner_node_id == gap.owner_node_id && gap.owned_start_us <= merged.back().owned_end_us) {
                merged.back().owned_end_us = std::max(merged.back().owned_end_us, gap.owned_end_us);
            }
            else merged.push_back(gap);
        }
        return merged;
    };
    output.source_control_gap_slices = merge_gaps(std::move(output.source_control_gap_slices));
    const auto projected = source.project_foreground_gap_across_logical_input_lanes(output.source_control_gap_slices);
    output.source_control_gap_slices.insert(output.source_control_gap_slices.end(), projected.begin(), projected.end());
    output.source_control_gap_slices = merge_gaps(std::move(output.source_control_gap_slices));
    std::ranges::sort(output.source_control_gap_slices, [](const auto & left, const auto & right) {
        if (left.owner_node_id != right.owner_node_id) return left.owner_node_id < right.owner_node_id;
        if (left.owned_start_us != right.owned_start_us) return left.owned_start_us < right.owned_start_us;
        return left.owned_end_us < right.owned_end_us;
    });
    for (const auto & gap : output.source_control_gap_slices) {
        output.source_control_gap_duration_us = core::checked_add_u64(output.source_control_gap_duration_us,
                                                                      gap.owned_duration_us(),
                                                                      "HiCache source CPU-control gap duration exceeds uint64 range");
    }
}

void assign_carrier_nodes(const HiCacheSourceDagIndex & source, std::vector<size_t> carrier_nodes, std::string reason, HiCacheSourceAttribution & output) {
    sort_unique(carrier_nodes);
    if (carrier_nodes.empty()) return;
    const std::unordered_set<size_t> carrier_set(carrier_nodes.begin(), carrier_nodes.end());
    output.source_carrier_state = model::HiCacheSourceCarrierState::Present;
    output.carrier_nodes = std::move(carrier_nodes);
    output.owned_duration_nodes = output.carrier_nodes;
    output.reason = std::move(reason);

    std::optional<size_t> earliest;
    std::optional<size_t> latest;
    for (size_t node_id : output.carrier_nodes) {
        const auto & event = source.graph().event_for_node(node_id);
        if (!earliest || event.ts < source.graph().event_for_node(*earliest).ts) earliest = node_id;
        const auto event_end = fact_end(HiCacheSourceFactNode{ .timestamp_us = event.ts, .duration_us = event.dur });
        const auto latest_end = latest ? fact_end(HiCacheSourceFactNode{ .timestamp_us = source.graph().event_for_node(*latest).ts,
                                                                         .duration_us = source.graph().event_for_node(*latest).dur })
                                       : 0;
        if (!latest || event_end > latest_end) latest = node_id;
        for (size_t edge_index : source.incoming_edge_ids(node_id)) {
            const auto & edge = source.graph().edge(edge_index);
            if (!carrier_set.contains(edge.src)) output.carrier_entry_edges.push_back(edge_index);
        }
        for (size_t edge_index : source.outgoing_edge_ids(node_id)) {
            const auto & edge = source.graph().edge(edge_index);
            if (!carrier_set.contains(edge.dst)) output.carrier_exit_edges.push_back(edge_index);
        }
    }
    sort_unique(output.carrier_entry_edges);
    sort_unique(output.carrier_exit_edges);
    output.start_anchor = earliest;
    output.completion_anchor = latest;
}

void copy_completion_wait_contract(const HiCacheIoOperationRecord & operation, HiCacheSourceAttribution & output) {
    output.observed_span_semantics = operation.observed_span_semantics;
    output.completion_wait_status = operation.completion_wait_status;
    output.completion_wait_reason = operation.completion_wait_reason;
    output.completion_join_contract_ready = operation.completion_join_contract_ready;
    output.source_readiness_topology_ready = operation.source_readiness_topology_ready;
    output.source_completion_wait_blocking = operation.source_completion_wait_blocking;
    output.control_ready_us = operation.control_ready_us;
    output.source_completion_us = operation.source_completion_us;
    output.wait_exit_start_us = operation.wait_exit_start_us;
    output.wait_exit_end_us = operation.wait_exit_end_us;
    output.completion_wait_duration_us = operation.completion_wait_duration_us;
    output.completion_wait_gap_duration_us = operation.completion_wait_gap_duration_us;
    output.logical_input_completion_wait_duration_us = operation.logical_input_completion_wait_duration_us;
    output.polling_lag_us = operation.polling_lag_us;
    output.retained_terminal_control_us = operation.retained_terminal_control_us;
    output.control_ready_anchor_node_id = operation.control_ready_anchor_node_id;
    output.wait_exit_anchor_node_id = operation.wait_exit_anchor_node_id;
    output.terminal_control_anchor_node_id = operation.terminal_control_anchor_node_id;
    output.completion_wait_owned_node_ids = operation.completion_wait_owned_node_ids;
    output.source_completion_node_ids = operation.device_completion_node_ids;
    output.readiness_join_node_ids = operation.readiness_join_node_ids;
    output.completion_wait_slices = operation.completion_wait_slices;
    output.logical_input_completion_wait_slices = operation.logical_input_completion_wait_slices;
    output.logical_input_completion_wait_duration_us = operation.logical_input_completion_wait_duration_us;
    if (!operation.logical_input_completion_wait_slices.empty()) output.evidence.push_back("logical_input_completion_wait_projection");
}

} // namespace markov::trace_graph::modules::hicache::patch::attribution_detail
