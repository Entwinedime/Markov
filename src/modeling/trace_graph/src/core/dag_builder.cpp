/**
 * @file
 * @brief Builds a TraceGraph DAG from Chrome trace duration events.
 *
 * Construction extracts only trace-supported execution order, submission chains,
 * and synchronization. What-if modules and HiCache policy run after the base DAG.
 */
#include "markov/trace_graph/core/dag_builder.hpp"

#include "dag_builder_stages.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace markov::trace_graph::core {

namespace dag_builder_detail {

uint64_t node_end_ts(const TraceEvent & event) { return checked_add_u64(event.ts, event.dur, "trace event end timestamp overflow"); }

bool is_usable_lane_value(const std::string & value) { return !value.empty() && value != "-1"; }

bool raw_contains_key_hint(const TraceEvent & event, std::string_view key) { return event.args_json_view().find(key) != std::string_view::npos; }

bool is_hicache_fact_event(const TraceEvent & event) { return event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact"); }

bool is_hicache_control_event(const TraceEvent & event) {
    return event.name.starts_with("hicache.control.") && !event.name.ends_with(".self") && event.ph == 'X';
}

struct ExecutionAndFactEvents {
    std::vector<TraceEvent> executable_events;
    std::vector<TraceEvent> hicache_fact_events;
};

std::vector<DagControlExclusionInterval> control_exclusion_intervals(const std::vector<TraceEvent> & events, int gpu_id) {
    std::vector<const TraceEvent *> extends;
    std::vector<const TraceEvent *> decodes;
    std::vector<DagControlExclusionInterval> intervals;
    for (const auto & event : events) {
        if (event.source_channel == TraceSourceChannel::Torch && event.ph == 'X' && event.pid == event.tid) {
            if (event.name.starts_with("step[EXTEND")) extends.push_back(&event);
            else if (event.name.starts_with("step[DECODE")) decodes.push_back(&event);
        }
        const bool profiling_snapshot =
            event.name.ends_with(":state_snapshot") || (event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("state_snapshot"));
        if (event.ph == 'X' && profiling_snapshot && event.dur > 0) {
            intervals.push_back(DagControlExclusionInterval{
                .gpu_id = gpu_id,
                .start_us = event.ts,
                .end_us = node_end_ts(event),
                .kind = DagControlExclusionKind::ProfilingSnapshot,
            });
        }
    }
    const auto event_order = [](const TraceEvent * left, const TraceEvent * right) {
        if (left->ts != right->ts) return left->ts < right->ts;
        return left->index < right->index;
    };
    std::ranges::sort(extends, event_order);
    std::ranges::sort(decodes, event_order);
    size_t decode_index = 0;
    for (const auto * extend : extends) {
        while (decode_index < decodes.size() && node_end_ts(*decodes[decode_index]) <= extend->ts) ++decode_index;
        if (decode_index >= decodes.size()) break;
        const auto end_us = node_end_ts(*decodes[decode_index++]);
        if (end_us <= extend->ts) continue;
        intervals.push_back(DagControlExclusionInterval{
            .gpu_id = gpu_id,
            .start_us = extend->ts,
            .end_us = end_us,
            .kind = DagControlExclusionKind::PrefillDecode,
        });
    }
    return intervals;
}

ExecutionAndFactEvents split_hicache_fact_events(std::vector<TraceEvent> events) {
    ExecutionAndFactEvents split;
    split.executable_events.reserve(events.size());
    for (auto & event : events) {
        if (is_hicache_fact_event(event)) split.hicache_fact_events.push_back(std::move(event));
        else split.executable_events.push_back(std::move(event));
    }
    return split;
}

std::optional<std::string> raw_hinted_arg(const TraceEvent & event, std::string_view key) {
    if (!raw_contains_key_hint(event, key)) return std::nullopt;
    return event.find_arg(key);
}

/**
 * @brief Resolves device classification, ordering lane, and optional sync aliases once.
 *
 * `Physic Stream Id` is the strongest device evidence. Kernel and cpu_op events may
 * instead use `streamId`. Device ordering prefers top-level `tid`, then `streamId`,
 * alternate `stream id`, and finally the physical stream ID.
 */
EventLaneIdentity resolve_event_lane(const TraceEvent & event, bool collect_aliases) {
    EventLaneIdentity identity;
    identity.physical_stream_id = raw_hinted_arg(event, "Physic Stream Id");
    const bool stream_identifies_device = event.cat == "Kernel" || event.cat == "cpu_op";
    if (stream_identifies_device) identity.stream_id = raw_hinted_arg(event, "streamId");
    identity.is_device = identity.physical_stream_id.has_value() || (stream_identifies_device && identity.stream_id.has_value());
    if (!identity.is_device) {
        identity.lane = "CPU:" + event.pid + ":" + event.tid;
        return identity;
    }

    if (collect_aliases || !is_usable_lane_value(event.tid)) {
        if (!identity.stream_id) identity.stream_id = raw_hinted_arg(event, "streamId");
        identity.alternate_stream_id = raw_hinted_arg(event, "stream id");
    }
    if (is_usable_lane_value(event.tid)) identity.lane = event.tid;
    else if (identity.stream_id && is_usable_lane_value(*identity.stream_id)) identity.lane = *identity.stream_id;
    else if (identity.alternate_stream_id && is_usable_lane_value(*identity.alternate_stream_id)) identity.lane = *identity.alternate_stream_id;
    else if (identity.physical_stream_id && is_usable_lane_value(*identity.physical_stream_id)) identity.lane = *identity.physical_stream_id;
    else identity.lane = "NPU_UNKNOWN";
    return identity;
}

} // namespace dag_builder_detail

DagBuilder::DagBuilder(size_t threads) : threads_(std::max<size_t>(1, threads)) {}

DagGraph DagBuilder::build(std::vector<TraceEvent> events, int gpu_id) const {
    auto parsed_count = events.size();
    auto exclusions = dag_builder_detail::control_exclusion_intervals(events, gpu_id);
    auto split = dag_builder_detail::split_hicache_fact_events(std::move(events));
    auto normalized = normalize_events(std::move(split.executable_events));
    DagGraph graph(std::move(normalized), gpu_id);
    graph.set_parsed_record_count(parsed_count);
    graph.set_hicache_fact_events(std::move(split.hicache_fact_events));
    graph.set_control_exclusion_intervals(std::move(exclusions));
    graph.reserve(DagGraphCapacity{ .nodes = graph.events().size(), .edges = 0 });

    auto index = create_node_index(graph);
    graph.reserve(DagGraphCapacity{ .nodes = graph.node_count(), .edges = estimate_edge_capacity(graph, index) });
    add_correlation_edges(graph, index, threads_);
    add_sequential_edges(graph, index);
    add_request_boundary_edges(graph, index);
    add_event_wait_edges(graph, index);
    add_notify_wait_edges(graph, index);
    add_model_execute_edges(graph, index);
    add_stream_sync_edges(graph, index);
    add_event_sync_edges(graph, index);
    add_device_sync_edges(graph, index);
    finalize_sync_nodes(graph, index);
    return graph;
}

size_t estimate_edge_capacity(const DagGraph & graph, const DagBuildIndex & index) {
    size_t capacity = 0;
    auto add = [&](size_t value) {
        if (value > std::numeric_limits<size_t>::max() - capacity) throw std::length_error("base DAG edge-capacity overflow");
        capacity += value;
    };
    auto add_group_edges = [&](const auto & groups) {
        for (const auto & [key, nodes] : groups) {
            (void)key;
            if (nodes.size() > 1) add(nodes.size() - 1);
        }
    };

    size_t device_lane_count = 0;
    for (const auto & [lane_id, nodes] : index.lane_to_nodes) {
        (void)lane_id;
        if (nodes.empty()) continue;
        add(nodes.size() - 1);
        if (!graph.node(nodes.front()).is_cpu) device_lane_count++;
    }
    add_group_edges(index.correlation_to_nodes);
    add_group_edges(index.connection_to_nodes);
    add(index.event_wait_nodes.size());
    add(index.notify_wait_candidate_count);
    add(index.event_sync_nodes.size());

    const auto lane_fanout_nodes = checked_add_u64(checked_add_u64(static_cast<uint64_t>(index.stream_sync_nodes.size()),
                                                                   static_cast<uint64_t>(index.device_sync_nodes.size()),
                                                                   "base DAG sync-node count overflow"),
                                                   static_cast<uint64_t>(index.model_execute_nodes.size()),
                                                   "base DAG sync-node count overflow");
    const auto lane_fanout_edges = checked_multiply_u64(lane_fanout_nodes, static_cast<uint64_t>(device_lane_count), "base DAG lane-fanout overflow");
    if (lane_fanout_edges > std::numeric_limits<size_t>::max()) throw std::length_error("base DAG edge-capacity overflow");
    add(static_cast<size_t>(lane_fanout_edges));
    return capacity;
}

} // namespace markov::trace_graph::core
