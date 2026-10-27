/**
 * @file
 * @brief Builds a TraceGraph DAG from Chrome trace duration events.
 *
 * Construction extracts only trace-supported execution order, submission chains,
 * and synchronization. What-if modules and HiCache policy run after the base DAG.
 */
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"

#include "markov/trace_graph/core/dag_builder_stages.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace markov::trace_graph::core {

namespace dag_builder_detail {

uint64_t node_end_ts(const TraceEvent & event) { return checked_add_u64(event.ts, event.dur, "trace event end timestamp overflow"); }

bool is_submit_anchor_event(const TraceEvent & event) { return event.submit_anchor; }

bool is_usable_lane_value(const std::string & value) { return !value.empty() && value != "-1"; }

bool is_control_envelope(const TraceEvent & event) {
    return event.call_role == TraceCallRole::ControlEnvelope || event.call_role == TraceCallRole::HostLoadEnvelope;
}

struct SourceEventGroups {
    std::vector<TraceEvent> executable_events;
    std::vector<TraceEvent> hicache_fact_events;
    std::vector<TraceEvent> request_observations;
    std::vector<TraceEvent> runtime_observations;
};

void retain_modeled_operator_envelopes(SourceEventGroups & split) {
    using Interval = std::pair<uint64_t, uint64_t>;
    std::map<std::pair<std::string, std::string>, std::vector<Interval>> lanes;
    const auto interval = [](const TraceEvent & event) { return Interval{ event.start_ns(), event.end_ns() }; };
    for (const auto & event : split.runtime_observations) {
        // Phase probes carry source geometry, not execution nodes. Retain
        // nested Torch envelopes for every such descriptor; individual
        // operation readers decide which implementations they understand.
        if (event.call_role == TraceCallRole::ModelWork) lanes[{ event.pid, event.tid }].push_back(interval(event));
    }
    for (auto & [lane, calls] : lanes) {
        std::ranges::sort(calls);
        std::vector<Interval> merged;
        for (const auto & call : calls) {
            if (merged.empty() || merged.back().second < call.first) merged.push_back(call);
            else merged.back().second = std::max(merged.back().second, call.second);
        }
        calls = std::move(merged);
    }

    // Normalization keeps CPU leaves, not their Torch operator envelopes.
    // Retain the latter as source-only observations so later cost extraction
    // can recover submission ownership without counting the envelope again.
    // Shape-bearing envelopes also preserve ordinary compute geometry, even
    // outside HiCache-specific probes. They remain observations, not extra
    // execution nodes. Shape-free traces retain only the modeled regions.
    for (const auto & event : split.executable_events) {
        if (event.source_channel != TraceSourceChannel::Torch || event.ph != 'X' || event.cat != "cpu_op") continue;
        if (event.has_arg("Input Dims")) {
            split.runtime_observations.push_back(event);
            continue;
        }
        const auto lane = lanes.find({ event.pid, event.tid });
        if (lane == lanes.end()) continue;

        const auto span = interval(event);
        auto call = std::ranges::upper_bound(lane->second, span.first, {}, &Interval::first);
        if (call == lane->second.begin()) continue;
        --call;
        if (span.second <= call->second) split.runtime_observations.push_back(event);
    }
}

std::vector<DagControlExclusionInterval> control_exclusion_intervals(const std::vector<TraceEvent> & events, const std::vector<DagPhaseMarkerEvent> & markers,
                                                                     int gpu_id) {
    std::vector<DagControlExclusionInterval> intervals;
    for (const auto & event : events) {
        if (event.ph == 'X' && event.snapshot_overhead && event.dur > 0) {
            intervals.push_back(DagControlExclusionInterval{
                .gpu_id = gpu_id,
                .start_us = event.ts,
                .end_us = node_end_ts(event),
                .kind = DagControlExclusionKind::ProfilingSnapshot,
            });
        }
    }

    // The phase side table already owns source-order marker selection. Reuse it
    // for the control metric instead of independently selecting and sorting phases.
    std::vector<const TraceEvent *> decodes;
    for (const auto & marker : markers) {
        if (marker.event.call_role == TraceCallRole::DecodeMarker) decodes.push_back(&marker.event);
    }

    size_t decode_index = 0;
    for (const auto & marker : markers) {
        const auto & extend = marker.event;
        if (extend.call_role != TraceCallRole::PrefillMarker) continue;

        while (decode_index < decodes.size() && node_end_ts(*decodes[decode_index]) <= extend.ts) ++decode_index;
        if (decode_index >= decodes.size()) break;
        const auto end_us = node_end_ts(*decodes[decode_index++]);
        if (end_us <= extend.ts) continue;
        intervals.push_back(DagControlExclusionInterval{
            .gpu_id = gpu_id,
            .start_us = extend.ts,
            .end_us = end_us,
            .kind = DagControlExclusionKind::PrefillDecode,
        });
    }
    return intervals;
}

std::vector<DagPhaseMarkerEvent> phase_marker_events(const std::vector<TraceEvent> & events, int gpu_id) {
    std::vector<DagPhaseMarkerEvent> markers;
    for (const auto & event : events) {
        if (event.call_role != TraceCallRole::PrefillMarker && event.call_role != TraceCallRole::DecodeMarker) continue;
        markers.push_back(DagPhaseMarkerEvent{ .gpu_id = gpu_id, .event = event });
    }
    std::ranges::sort(markers, [](const auto & left, const auto & right) {
        if (left.event.ts != right.event.ts) return left.event.ts < right.event.ts;
        return left.event.index < right.event.index;
    });
    return markers;
}

SourceEventGroups split_observation_events(std::vector<TraceEvent> events) {
    SourceEventGroups split;
    split.executable_events.reserve(events.size());
    for (auto & event : events) {
        // An admission envelope supplies observations even when its children
        // cover all self time; it still participates in leaf normalization.
        if (event.call_role == TraceCallRole::HostLoadEnvelope) split.runtime_observations.push_back(event);

        switch (event.routing) {
        case TraceRouting::RuntimeObservation:
            split.runtime_observations.push_back(std::move(event));
            break;
        case TraceRouting::RequestBegin:
            // Only the start is a point. The separately observed dispatch
            // boundary supplies the end; the envelope is not extra service.
            event.name += ".begin";
            event.dur = 0;
            event.dur_submicro_ns = 0;
            split.request_observations.push_back(std::move(event));
            break;
        case TraceRouting::RequestObservation:
            split.request_observations.push_back(std::move(event));
            break;
        case TraceRouting::CacheFact:
            split.hicache_fact_events.push_back(std::move(event));
            break;
        case TraceRouting::Execution:
            split.executable_events.push_back(std::move(event));
            break;
        case TraceRouting::Ignore:
            break;
        }
    }
    return split;
}

std::optional<std::string> hinted_arg(const TraceEvent & event, std::string_view key) {
    if (!event.has_arg_key_hint(key)) return std::nullopt;
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
    identity.physical_stream_id = hinted_arg(event, "Physic Stream Id");
    const bool stream_identifies_device = event.cat == "Kernel" || event.cat == "cpu_op";
    if (stream_identifies_device) identity.stream_id = hinted_arg(event, "streamId");
    identity.is_device = identity.physical_stream_id.has_value() || (stream_identifies_device && identity.stream_id.has_value());
    if (!identity.is_device) {
        identity.lane = "CPU:" + event.pid + ":" + event.tid;
        return identity;
    }

    if (collect_aliases || !is_usable_lane_value(event.tid)) {
        if (!identity.stream_id) identity.stream_id = hinted_arg(event, "streamId");
        identity.alternate_stream_id = hinted_arg(event, "stream id");
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
    auto phase_markers = dag_builder_detail::phase_marker_events(events, gpu_id);
    auto exclusions = dag_builder_detail::control_exclusion_intervals(events, phase_markers, gpu_id);
    auto split = dag_builder_detail::split_observation_events(std::move(events));
    dag_builder_detail::retain_modeled_operator_envelopes(split);
    auto normalized = normalize_events(std::move(split.executable_events), split.runtime_observations);
    DagGraph graph(std::move(normalized), gpu_id);
    graph.set_parsed_record_count(parsed_count);
    graph.set_hicache_fact_events(std::move(split.hicache_fact_events));
    graph.set_runtime_observations(std::move(split.runtime_observations));
    graph.set_control_exclusion_intervals(std::move(exclusions));
    graph.set_phase_marker_events(std::move(phase_markers));
    graph.reserve(DagGraphCapacity{ .nodes = graph.events().size(), .edges = 0 });

    auto index = create_node_index(graph);
    graph.reserve(DagGraphCapacity{ .nodes = graph.node_count(), .edges = estimate_edge_capacity(graph, index) });
    add_correlation_edges(graph, index, threads_);
    add_sequential_edges(graph, index);
    add_event_wait_edges(graph, index);
    add_notify_wait_edges(graph, index);
    add_model_execute_edges(graph, index);
    add_sync_edges(graph, index);
    finalize_sync_nodes(graph, index);
    normalize_cpu_queue_waits(graph);
    std::ranges::sort(split.request_observations, {}, &TraceEvent::ts);
    // These boundaries carry no extra cost or E2E eligibility. A future response
    // path must separately prove complete request binding before using them.
    insert_cpu_gap_observations(graph, split.request_observations);
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
