/** @file Partition observed CPU gaps into connectable, zero-cost boundaries. */
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::core {
namespace {

size_t add_boundary(DagGraph & graph, const TraceEvent & observation, const std::string & lane, int rank, std::string_view suffix, uint64_t timestamp,
                    uint64_t gap) {
    TraceArgMap attrs = observation.args_map();
    attrs["observed_interval"] = observation.name;
    attrs["interval_boundary"] = suffix;
    const auto id = graph.add_synthetic_node({
        .name = observation.name + "." + std::string(suffix),
        .category = "observed_boundary",
        .lane_key = lane,
        .attrs = std::move(attrs),
    });
    auto & event = graph.mutable_event_for_node(id);
    event.ts = timestamp;
    event.pid = observation.pid;
    event.tid = observation.tid;
    auto & node = graph.mutable_node(id);
    node.gpu_id = rank;
    node.observed_cpu_coordinates = true;
    node.cpu_gap_after = node.original_cpu_gap_after = gap;
    return id;
}

CpuGapBoundaryNodes split_gap(DagGraph & graph, size_t edge_id, const TraceEvent & observation) {
    const auto edge = graph.edge(edge_id);
    const auto finish = observation.ts + observation.dur;
    const auto gap_start = graph.event_for_node(edge.src).ts + graph.event_for_node(edge.src).dur;
    const auto gap_end = graph.event_for_node(edge.dst).ts;
    const auto lane = std::string(graph.node_lane_key(edge.src));
    const auto rank = graph.node(edge.src).gpu_id;
    const auto boundary = [&](std::string_view suffix, uint64_t timestamp, uint64_t gap) {
        return add_boundary(graph, observation, lane, rank, suffix, timestamp, gap);
    };
    const bool instant = observation.dur == 0;
    const auto begin = boundary(instant ? "point" : "begin", observation.ts, instant ? gap_end - finish : observation.dur);
    const auto end = instant ? begin : boundary("end", finish, gap_end - finish);
    auto & source = graph.mutable_node(edge.src);
    source.cpu_gap_after = source.original_cpu_gap_after = observation.ts - gap_start;
    graph.disable_edge(edge_id);
    graph.add_edge(edge.src, begin, DagEdgeKind::Sequential);
    if (!instant) graph.add_edge(begin, end, DagEdgeKind::Sequential);
    graph.add_edge(end, edge.dst, DagEdgeKind::Sequential);
    return { begin, end };
}

std::optional<std::pair<uint64_t, uint64_t>> unchanged_cpu_gap(const DagGraph & graph, size_t edge_id) {
    const auto & edge = graph.edge(edge_id);
    if (!edge.active || edge.kind != DagEdgeKind::Sequential) return std::nullopt;
    const auto & source = graph.node(edge.src);
    const auto & target = graph.node(edge.dst);
    if (!source.active || !target.active || !source.is_cpu || !target.is_cpu || source.lane_id != target.lane_id) return std::nullopt;

    const auto & before = graph.event_for_node(edge.src);
    const auto & after = graph.event_for_node(edge.dst);
    if (before.cat == "observed_boundary" && before.arg("interval_boundary") == "begin") return std::nullopt;
    if (before.pid != after.pid || before.tid != after.tid) return std::nullopt;

    const auto begin = checked_add_u64(before.ts, before.dur, "CPU gap start overflow");
    if (begin > after.ts || source.cpu_gap_after != after.ts - begin || graph.scope_gap_duration(edge.src) != 0) return std::nullopt;
    return std::pair{ begin, after.ts };
}

} // namespace

void insert_cpu_gap_observations(DagGraph & graph, std::span<const TraceEvent> observations) {
    // Own the observations: graph insertion can reallocate the event storage.
    const std::vector<TraceEvent> intervals(observations.begin(), observations.end());
    std::vector<uint64_t> finish_us;
    finish_us.reserve(intervals.size());
    using LaneKey = std::pair<std::string_view, std::string_view>;
    std::map<LaneKey, std::vector<size_t>> lanes;
    for (size_t i = 0; i < intervals.size(); ++i) {
        const auto & observation = intervals[i];
        finish_us.push_back(checked_add_u64(observation.ts, observation.dur, "observed interval end overflow"));
        if (!observation.pid.empty() && !observation.tid.empty() && observation.pid != "-1" && observation.tid != "-1")
            lanes[{ observation.pid, observation.tid }].push_back(i);
    }
    if (lanes.empty()) return;
    for (auto & [lane, ids] : lanes) std::ranges::stable_sort(ids, {}, [&](size_t id) { return intervals[id].ts; });

    const auto original_edge_count = graph.edge_count();
    std::vector<size_t> sequential_count(graph.node_count(), 0);
    std::vector<std::vector<size_t>> candidates(intervals.size());
    for (size_t id = 0; id < original_edge_count; ++id) {
        const auto & edge = graph.edge(id);
        if (edge.active && edge.kind == DagEdgeKind::Sequential) ++sequential_count[edge.src];
        const auto gap = unchanged_cpu_gap(graph, id);
        if (!gap) continue;
        const auto & before = graph.event_for_node(edge.src);
        const auto lane = lanes.find({ before.pid, before.tid });
        if (lane == lanes.end()) continue;

        const auto & ids = lane->second;
        for (auto point = std::ranges::lower_bound(ids, gap->first, {}, [&](size_t i) { return intervals[i].ts; });
             point != ids.end() && intervals[*point].ts <= gap->second;
             ++point) {
            if (finish_us[*point] <= gap->second) candidates[*point].push_back(id);
        }
    }

    // Initial gaps can only be disabled or partitioned by this operation. Keep
    // their indexed candidates and also inspect the few newly created edges;
    // equal-time points must still detect ambiguity between adjacent pieces.
    for (size_t i = 0; i < intervals.size(); ++i) {
        auto & edges = candidates[i];
        for (size_t id = original_edge_count; id < graph.edge_count(); ++id) edges.push_back(id);
        const auto & observation = intervals[i];
        std::optional<size_t> selected;
        for (const auto id : edges) {
            const auto gap = unchanged_cpu_gap(graph, id);
            if (!gap || gap->first > observation.ts || finish_us[i] > gap->second) continue;
            const auto & before = graph.event_for_node(graph.edge(id).src);
            if (before.pid != observation.pid || before.tid != observation.tid) continue;
            if (selected) {
                selected.reset();
                break;
            }
            selected = id;
        }
        if (!selected) continue;

        const auto source = graph.edge(*selected).src;
        // Splitting preserves the original predecessor's Sequential degree;
        // every generated boundary has exactly one Sequential consumer. Other
        // dependency kinds do not inherit this gap and remain untouched.
        if (source < sequential_count.size() && sequential_count[source] != 1) continue;

        (void)split_gap(graph, *selected, observation);
    }
}

std::vector<std::optional<size_t>> insert_cpu_gap_points(DagGraph & graph, std::span<const TraceEvent> observations) {
    // Callers may select points from graph event storage; insertion can move local events.
    const std::vector<TraceEvent> points(observations.begin(), observations.end());
    std::vector<std::optional<size_t>> result(points.size()), selected(points.size());
    std::vector<bool> ambiguous(points.size(), false);
    using LaneKey = std::pair<std::string_view, std::string_view>;
    // Keys borrow from immutable points, not graph events that insertion can move.
    // Probing a large graph must not allocate pid/tid strings for every node.
    std::map<LaneKey, std::vector<size_t>> lanes;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto & point = points[i];
        if (point.dur == 0 && !point.pid.empty() && !point.tid.empty() && point.pid != "-1" && point.tid != "-1") lanes[{ point.pid, point.tid }].push_back(i);
    }
    if (lanes.empty()) return result;
    for (auto & [lane, ids] : lanes) std::ranges::stable_sort(ids, {}, [&](size_t id) { return points[id].ts; });
    std::vector<size_t> sequential_count(graph.node_count(), 0);
    std::unordered_set<size_t> worker_lanes;
    for (const auto & edge : graph.edges()) {
        if (!edge.active) continue;
        if (edge.kind == DagEdgeKind::Sequential) ++sequential_count[edge.src];
        const auto &source = graph.node(edge.src), &target = graph.node(edge.dst);
        if (edge.kind == DagEdgeKind::Correlation && source.is_cpu && target.is_cpu && source.lane_id != target.lane_id) worker_lanes.insert(target.lane_id);
    }
    struct Tail {
        size_t node;
        uint64_t end;
        bool multiple_lanes = false, tied_end = false;
    };
    std::map<LaneKey, Tail> tails;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu) continue;
        const auto & event = graph.event_for_node(node.id);
        const auto lane = lanes.find(LaneKey{ event.pid, event.tid });
        if (lane == lanes.end()) continue;
        const auto end = checked_add_u64(event.ts, event.dur, "CPU lane end overflow");
        auto [entry, inserted] = tails.try_emplace(lane->first, Tail{ node.id, end });
        if (inserted) continue;
        auto & tail = entry->second;
        tail.multiple_lanes |= graph.node(tail.node).lane_id != node.lane_id;
        if (end == tail.end) tail.tied_end = true;
        else if (end > tail.end) {
            tail.node = node.id;
            tail.end = end;
            tail.tied_end = false;
        }
    }
    for (size_t id = 0; id < graph.edge_count(); ++id) {
        const auto & edge = graph.edge(id);
        if (!edge.active || edge.kind != DagEdgeKind::Sequential || sequential_count[edge.src] != 1) continue;
        const auto &source = graph.node(edge.src), &target = graph.node(edge.dst);
        if (!source.active || !target.active || !source.is_cpu || !target.is_cpu || source.lane_id != target.lane_id || source.explicit_cpu_task
            || target.explicit_cpu_task || worker_lanes.contains(source.lane_id))
            continue;
        const auto &before = graph.event_for_node(edge.src), &after = graph.event_for_node(edge.dst);
        const auto lane = lanes.find({ before.pid, before.tid });
        if (lane == lanes.end() || after.pid != before.pid || after.tid != before.tid) continue;
        if (before.cat == "observed_boundary" && before.arg("interval_boundary") == "begin") continue;
        const auto begin = checked_add_u64(before.ts, before.dur, "CPU gap start overflow");
        if (begin > after.ts || source.cpu_gap_after != after.ts - begin || graph.scope_gap_duration(edge.src) != 0) continue;
        const auto & ids = lane->second;
        for (auto point = std::ranges::lower_bound(ids, begin, {}, [&](size_t i) { return points[i].ts; }); point != ids.end() && points[*point].ts <= after.ts;
             ++point) {
            if (selected[*point]) ambiguous[*point] = true;
            selected[*point] = id;
        }
    }
    std::map<size_t, std::vector<size_t>> by_edge;
    for (size_t i = 0; i < points.size(); ++i)
        if (selected[i] && !ambiguous[i]) by_edge[*selected[i]].push_back(i);
    for (auto & [edge, ids] : by_edge) {
        std::ranges::stable_sort(ids, {}, [&](size_t id) { return points[id].ts; });
        auto remaining = edge;
        std::optional<size_t> previous;
        for (const auto id : ids) {
            if (previous && points[*previous].ts == points[id].ts) result[id] = result[*previous];
            else {
                result[id] = split_gap(graph, remaining, points[id]).begin;
                remaining = graph.edge_count() - 1; // split_gap appends the remaining suffix last.
            }
            previous = id;
        }
    }
    // Window clipping can remove the next native CPU call, while a semantic
    // return still proves a later point on the same thread. Retain that tail
    // as gap, not service or an extra business endpoint.
    for (const auto & [lane, tail] : tails) {
        const auto & node = graph.node(tail.node);
        const auto & event = graph.event_for_node(tail.node);
        if (tail.multiple_lanes || tail.tied_end || sequential_count[tail.node] || node.explicit_cpu_task || worker_lanes.contains(node.lane_id)
            || node.duration != event.dur || node.cpu_gap_after || node.original_cpu_gap_after || graph.scope_gap_duration(tail.node))
            continue;
        const auto lane_key = std::string(graph.node_lane_key(tail.node));
        const auto rank = node.gpu_id;
        auto current = tail.node;
        auto current_end = tail.end;
        std::optional<size_t> previous;
        for (const auto id : lanes.at(lane)) {
            if (selected[id] || ambiguous[id] || points[id].ts < tail.end) continue;
            if (previous && points[*previous].ts == points[id].ts) result[id] = result[*previous];
            else {
                const auto point = add_boundary(graph, points[id], lane_key, rank, "point", points[id].ts, 0);
                auto & before = graph.mutable_node(current);
                before.cpu_gap_after = before.original_cpu_gap_after = points[id].ts - current_end;
                graph.add_edge(current, point, DagEdgeKind::Sequential);
                result[id] = point;
                current = point;
                current_end = points[id].ts;
            }
            previous = id;
        }
    }
    return result;
}

} // namespace markov::trace_graph::core
