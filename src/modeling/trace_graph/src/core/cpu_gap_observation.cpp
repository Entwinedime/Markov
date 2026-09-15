/** @file Partition observed CPU gaps into connectable, zero-cost boundaries. */
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>

namespace markov::trace_graph::core {

std::optional<CpuGapBoundaryNodes> insert_cpu_gap_observation(DagGraph & graph, TraceEvent observation) {
    const auto finish = checked_add_u64(observation.ts, observation.dur, "observed interval end overflow");
    if (observation.pid.empty() || observation.tid.empty() || observation.pid == "-1" || observation.tid == "-1" || observation.dur == 0) return std::nullopt;
    std::optional<size_t> selected;
    for (size_t index = 0; index < graph.edges().size(); ++index) {
        const auto & edge = graph.edge(index);
        if (!edge.active || edge.kind != DagEdgeKind::Sequential) continue;
        const auto & source = graph.node(edge.src);
        const auto & target = graph.node(edge.dst);
        if (!source.active || !target.active || !source.is_cpu || !target.is_cpu || source.lane_id != target.lane_id) continue;
        const auto & before = graph.event_for_node(edge.src);
        const auto & after = graph.event_for_node(edge.dst);
        if (before.cat == "observed_boundary" && before.arg("interval_boundary") == "begin") continue;
        if (before.pid != observation.pid || before.tid != observation.tid || after.pid != observation.pid || after.tid != observation.tid) continue;
        const auto begin = checked_add_u64(before.ts, before.dur, "CPU gap start overflow");
        if (begin > observation.ts || finish > after.ts) continue;
        if (source.duration != before.dur || target.duration != after.dur || source.cpu_gap_after != after.ts - begin
            || graph.scope_gap_duration(edge.src) != 0) continue;
        if (selected) return std::nullopt;
        selected = index;
    }
    if (!selected) return std::nullopt;
    const auto edge = graph.edge(*selected);
    // The delay belongs to the source node, so shortening it would affect every
    // Sequential consumer. Non-Sequential consumers do not inherit this gap.
    if (std::ranges::count_if(graph.edges(), [&](const auto & candidate) {
            return candidate.active && candidate.src == edge.src && candidate.kind == DagEdgeKind::Sequential;
        }) != 1) return std::nullopt;

    const auto gap_start = graph.event_for_node(edge.src).ts + graph.event_for_node(edge.src).dur;
    const auto gap_end = graph.event_for_node(edge.dst).ts;
    const auto lane = std::string(graph.node_lane_key(edge.src));
    const auto boundary = [&](std::string_view suffix, uint64_t timestamp, uint64_t gap) {
        std::unordered_map<std::string, std::string> attrs(observation.args_map().begin(), observation.args_map().end());
        attrs["observed_interval"] = observation.name;
        attrs["interval_boundary"] = suffix;
        const auto id = graph.add_synthetic_node({
            .name = observation.name + "." + std::string(suffix), .category = "observed_boundary",
            .lane_key = lane, .attrs = std::move(attrs),
        });
        auto & event = graph.mutable_event_for_node(id);
        event.ts = timestamp;
        event.pid = observation.pid;
        event.tid = observation.tid;
        graph.mutable_node(id).cpu_gap_after = gap;
        graph.mutable_node(id).original_cpu_gap_after = gap;
        return id;
    };
    const auto begin = boundary("begin", observation.ts, observation.dur);
    const auto end = boundary("end", finish, gap_end - finish);
    graph.mutable_node(edge.src).cpu_gap_after = observation.ts - gap_start;
    // This is a partition of observed time, not a predicted duration change.
    // Exclusion overlaps must use the new subinterval rather than the old whole.
    graph.mutable_node(edge.src).original_cpu_gap_after = observation.ts - gap_start;
    graph.disable_edge(*selected);
    graph.add_edge(edge.src, begin, DagEdgeKind::Sequential);
    graph.add_edge(begin, end, DagEdgeKind::Sequential);
    graph.add_edge(end, edge.dst, DagEdgeKind::Sequential);
    return CpuGapBoundaryNodes{begin, end};
}

} // namespace markov::trace_graph::core
