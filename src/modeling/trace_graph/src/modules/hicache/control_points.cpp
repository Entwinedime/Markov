#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include <algorithm>
#include <tuple>

namespace markov::trace_graph::modules::hicache {

std::vector<std::optional<size_t>> bind_hicache_control_points(core::DagGraph & graph, std::span<const core::TraceEvent> points) {
    using Coordinate = std::tuple<std::string, std::string, uint64_t>;
    std::map<Coordinate, std::vector<size_t>> existing;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.duration) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.cat == "observed_boundary") existing[{ event.pid, event.tid, event.ts }].push_back(node.id);
    }
    std::vector<std::optional<size_t>> result(points.size());
    std::vector<core::TraceEvent> missing;
    std::vector<size_t> positions;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto & point = points[i];
        const auto found = existing.find({ point.pid, point.tid, point.ts });
        if (found != existing.end()) {
            if (found->second.size() == 1) result[i] = found->second.front();
        }
        else {
            positions.push_back(i);
            missing.push_back(point);
        }
    }
    const auto gaps = core::insert_cpu_gap_points(graph, missing);
    for (size_t i = 0; i < gaps.size(); ++i) result[positions[i]] = gaps[i];
    const patch::HiCacheSourceDagIndex source(graph);
    std::map<size_t, std::map<uint64_t, std::vector<size_t>>> splits;
    for (const auto i : positions) {
        if (result[i]) continue;
        const auto & point = points[i];
        if (const auto entry = source.cpu_node_starting_at(point.pid, point.tid, point.ts)) {
            result[i] = entry;
            continue;
        }
        std::vector<size_t> covering;
        for (const auto id : source.cpu_nodes_on_lane(point.pid, point.tid)) {
            const auto & event = graph.event_for_node(id);
            if (event.ts < point.ts && point.ts < event.ts + event.dur) covering.push_back(id);
        }
        if (covering.size() != 1) continue;
        const auto id = covering.front();
        const auto & node = graph.node(id);
        const auto & event = graph.event_for_node(id);
        if (event.arg("hicache_control_semantics") != "parent_self_time" || event.arg("hicache_control_parent").empty()
            || event.arg("hicache_control_parent_index").empty() || node.explicit_cpu_task || node.cpu_ready_delay_before
            || node.duration != node.original_duration || node.duration != event.dur || node.cpu_gap_after != node.original_cpu_gap_after
            || graph.scope_node_owned(id) || graph.scope_gap_duration(id))
            continue;
        if (std::ranges::any_of(source.incoming_edge_ids(id),
                                [&](size_t edge) { return graph.edge(edge).active && graph.edge(edge).kind == core::DagEdgeKind::Correlation; }))
            continue;
        splits[id][point.ts].push_back(i);
    }
    for (const auto & [id, cuts] : splits) {
        const auto original = graph.node(id);
        const auto event = graph.event_for_node(id);
        const auto lane = std::string(graph.node_lane_key(id));
        const std::unordered_map<std::string, std::string> attrs(event.args_map().begin(), event.args_map().end());
        uint64_t cursor = event.ts;
        std::optional<size_t> previous;
        for (const auto & [at, indices] : cuts) {
            const auto prefix = graph.add_synthetic_node({
                .name = event.name,
                .category = event.cat,
                .lane_key = lane,
                .duration = at - cursor,
                .attrs = attrs,
                .observed_point = core::DagObservedPoint{ event.pid, event.tid, cursor, original.gpu_id }
            });
            graph.mutable_node(prefix).observed_cpu_coordinates = true;
            if (previous) graph.add_edge(*previous, prefix, core::DagEdgeKind::Sequential);
            else
                for (const auto edge : source.incoming_edge_ids(id))
                    if (graph.edge(edge).active) graph.mutable_edge(edge).dst = prefix;
            const auto boundary = graph.add_synthetic_node({
                .name = points[indices.front()].name + ".point",
                .category = "observed_boundary",
                .lane_key = lane,
                .observed_point = core::DagObservedPoint{ event.pid, event.tid, at, original.gpu_id }
            });
            graph.add_edge(prefix, boundary, core::DagEdgeKind::Sequential);
            for (const auto i : indices) result[i] = boundary;
            cursor = at;
            previous = boundary;
        }
        // Keep the original ID at the original completion time, so existing
        // consumers and its residual tail gap keep exactly the same meaning.
        auto & tail = graph.mutable_event_for_node(id);
        tail.ts = cursor;
        tail.dur = event.ts + event.dur - cursor;
        graph.mutable_node(id).duration = graph.mutable_node(id).original_duration = tail.dur;
        graph.add_edge(*previous, id, core::DagEdgeKind::Sequential);
    }
    return result;
}

} // namespace markov::trace_graph::modules::hicache
