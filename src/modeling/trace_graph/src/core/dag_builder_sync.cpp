/**
 * @file
 * @brief Builds trace-supported synchronization boundaries.
 */
#include "dag_builder_stages.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace markov::trace_graph::core {

namespace dag_builder_detail {

bool is_stream_sync_event(const std::string & name) {
    return name == "AscendCL@aclrtSynchronizeStream" || name == "AscendCL@aclrtSynchronizeStreamWithTimeout";
}

bool is_event_sync_event(const std::string & name) { return name == "AscendCL@aclrtSynchronizeEvent" || name == "AscendCL@aclrtSynchronizeEventWithTimeout"; }

bool is_device_sync_event(const std::string & name) {
    return name == "AscendCL@aclrtSynchronizeDevice" || name == "AscendCL@aclrtSynchronizeDeviceWithTimeout";
}

} // namespace dag_builder_detail

namespace {

using dag_builder_detail::is_usable_lane_value;
using dag_builder_detail::node_end_ts;

uint64_t event_submit_ts(const DagGraph & graph, size_t node_id) {
    const auto & node = graph.node(node_id);
    return node.submit_ts > 0 ? node.submit_ts : graph.event_for_node(node_id).ts;
}

std::optional<std::string> event_id_from_cpu_record(const TraceEvent & event) {
    auto value = event.arg("Event Id");
    if (!value.empty()) return value;
    value = event.arg("event_id");
    if (!value.empty()) return value;
    return std::nullopt;
}

struct SubmitFrontierNode {
    size_t node_id = 0;
    uint64_t effective_submit_ts = 0;
};


std::unordered_map<size_t, std::vector<SubmitFrontierNode>> build_submit_frontiers(const DagGraph & graph,
                                                                                   const std::unordered_map<size_t, std::vector<size_t>> & lane_to_nodes) {
    std::unordered_map<size_t, std::vector<SubmitFrontierNode>> frontiers;
    frontiers.reserve(lane_to_nodes.size());
    for (const auto & item : lane_to_nodes) {
        if (item.second.empty() || graph.node(item.second.front()).is_cpu) continue;
        uint64_t effective_submit_ts = 0;
        auto & frontier = frontiers[item.first];
        frontier.reserve(item.second.size());
        for (size_t node_id : item.second) {
            const auto submit_ts = event_submit_ts(graph, node_id);
            effective_submit_ts = std::max(effective_submit_ts, submit_ts);
            frontier.push_back(SubmitFrontierNode{ .node_id = node_id, .effective_submit_ts = effective_submit_ts });
        }
    }
    return frontiers;
}

std::optional<size_t> find_submitted_frontier_node(const std::vector<SubmitFrontierNode> & frontier, uint64_t sync_ts) {
    auto bound = std::lower_bound(frontier.begin(), frontier.end(), sync_ts, [](const SubmitFrontierNode & node, uint64_t value) {
        return node.effective_submit_ts < value;
    });
    if (bound == frontier.begin()) return std::nullopt;
    --bound;
    return bound->node_id;
}


using NodeGroups = std::unordered_map<std::string, std::vector<size_t>>;
using LaneNodes = std::unordered_map<size_t, std::vector<size_t>>;
using StreamLaneMap = std::unordered_map<std::string, size_t>;
using SubmitFrontiers = std::unordered_map<size_t, std::vector<SubmitFrontierNode>>;

struct EventRecordBindings {
    const NodeGroups & connection_to_nodes;
    StreamLaneMap & raw_stream_to_lane;
    StreamLaneMap & stream_alias_to_lane;
    NodeGroups & event_id_to_nodes;
};

/** @brief Read-only identity indices used to resolve one event wait. */
struct EventWaitBindings {
    const NodeGroups & connection_to_nodes;
    const NodeGroups & event_id_to_nodes;
};

void bind_event_record(DagGraph & graph, size_t record_node, const EventRecordBindings & bindings) {
    const auto & record_event = graph.event_for_node(record_node);
    const auto connection = bindings.connection_to_nodes.find(record_event.arg("connection_id"));
    if (connection == bindings.connection_to_nodes.end() || connection->second.empty()) return;
    const auto & cpu_event = graph.event_for_node(connection->second.front());
    const auto raw_stream = cpu_event.arg("Raw Stream");
    if (!raw_stream.empty()) {
        const auto lane = graph.node(record_node).lane_id;
        bindings.raw_stream_to_lane[raw_stream] = lane;
        bindings.stream_alias_to_lane[raw_stream] = lane;
    }
    const auto event_id = event_id_from_cpu_record(cpu_event);
    if (event_id) bindings.event_id_to_nodes[*event_id].push_back(record_node);
}

void sort_event_records(const DagGraph & graph, NodeGroups & event_id_to_nodes) {
    for (auto & [event_id, nodes] : event_id_to_nodes) {
        (void)event_id;
        std::ranges::sort(nodes, [&](size_t left, size_t right) { return graph.event_for_node(left).ts < graph.event_for_node(right).ts; });
    }
}

std::optional<size_t> latest_cross_lane_record(const DagGraph & graph, size_t wait_node, const std::vector<size_t> & records) {
    const auto & wait_event = graph.event_for_node(wait_node);
    auto bound = records.end();
    if (wait_event.dur == 0) {
        const auto bound_value = wait_event.ts > 0 ? wait_event.ts - 1 : 0;
        bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](uint64_t value, size_t node_id) {
            return value < graph.event_for_node(node_id).ts;
        });
    }
    else {
        const auto bound_value = static_cast<double>(wait_event.ts) + static_cast<double>(wait_event.dur) - 0.1;
        bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](double value, size_t node_id) {
            return value < static_cast<double>(graph.event_for_node(node_id).ts);
        });
    }
    if (bound == records.begin()) return std::nullopt;
    --bound;
    const auto wait_lane = graph.node(wait_node).lane_id;
    while (bound != records.begin() && wait_lane == graph.node(*bound).lane_id) --bound;
    if (wait_lane == graph.node(*bound).lane_id) return std::nullopt;
    return *bound;
}

std::optional<std::string> wait_event_id(const DagGraph & graph, size_t wait_node, const NodeGroups & connection_to_nodes) {
    const auto & wait_event = graph.event_for_node(wait_node);
    const auto connection = connection_to_nodes.find(wait_event.arg("connection_id"));
    if (connection == connection_to_nodes.end() || connection->second.empty()) return std::nullopt;
    return event_id_from_cpu_record(graph.event_for_node(connection->second.front()));
}

void add_event_wait_dependency(DagGraph & graph, size_t wait_node, const EventWaitBindings & bindings) {
    const auto event_id = wait_event_id(graph, wait_node, bindings.connection_to_nodes);
    if (!event_id) return;
    const auto records = bindings.event_id_to_nodes.find(*event_id);
    if (records == bindings.event_id_to_nodes.end()) return;
    const auto record = latest_cross_lane_record(graph, wait_node, records->second);
    if (record) graph.add_edge(*record, wait_node, DagEdgeKind::Sync);
}

uint64_t earliest_notify_end(const DagGraph & graph, uint64_t model_start, const std::vector<size_t> & notify_wait_nodes) {
    uint64_t notify_end = model_start;
    for (const auto wait_node : notify_wait_nodes) {
        const auto & wait_event = graph.event_for_node(wait_node);
        if (wait_event.ts < model_start) continue;
        const auto end = node_end_ts(wait_event);
        if (notify_end == model_start || end < notify_end) notify_end = end;
    }
    return notify_end;
}

std::optional<size_t> first_node_in_window(const DagGraph & graph, const std::vector<size_t> & nodes, uint64_t begin, uint64_t end) {
    for (const auto node_id : nodes) {
        const auto timestamp = graph.event_for_node(node_id).ts;
        if (timestamp >= begin && timestamp <= end) return node_id;
    }
    return std::nullopt;
}

std::optional<size_t> resolve_stream_lane(const DagGraph & graph, const LaneNodes & lane_to_nodes, const StreamLaneMap & raw_stream_to_lane,
                                          const StreamLaneMap & stream_alias_to_lane, const std::string & stream_id) {
    std::optional<size_t> lane;
    if (const auto raw = raw_stream_to_lane.find(stream_id); raw != raw_stream_to_lane.end()) lane = raw->second;
    if (!lane) {
        if (const auto alias = stream_alias_to_lane.find(stream_id); alias != stream_alias_to_lane.end()) lane = alias->second;
    }
    if (!lane) lane = graph.find_lane_id(stream_id);
    if (lane && lane_to_nodes.contains(*lane)) return lane;
    return std::nullopt;
}

std::vector<size_t> stream_sync_target_lanes(const DagGraph & graph, const TraceEvent & sync_event, const LaneNodes & lane_to_nodes,
                                             const StreamLaneMap & raw_stream_to_lane, const StreamLaneMap & stream_alias_to_lane) {
    std::vector<size_t> lanes;
    std::unordered_set<size_t> seen;
    bool has_stream_evidence = false;
    constexpr std::string_view keys[] = { "Raw Stream", "streamId", "stream id", "Physic Stream Id" };
    for (const auto key : keys) {
        const auto stream_id = sync_event.arg(key);
        if (!is_usable_lane_value(stream_id)) continue;
        has_stream_evidence = true;
        const auto lane = resolve_stream_lane(graph, lane_to_nodes, raw_stream_to_lane, stream_alias_to_lane, stream_id);
        if (lane && seen.insert(*lane).second) lanes.push_back(*lane);
    }
    if (has_stream_evidence) return lanes;
    for (const auto & [lane_id, nodes] : lane_to_nodes) {
        if (!nodes.empty() && !graph.node(nodes.front()).is_cpu) lanes.push_back(lane_id);
    }
    return lanes;
}

void add_stream_sync_dependency(DagGraph & graph, size_t sync_node, const LaneNodes & lane_to_nodes, const StreamLaneMap & raw_stream_to_lane,
                                const StreamLaneMap & stream_alias_to_lane, const SubmitFrontiers & submit_frontiers) {
    const auto & sync_event = graph.event_for_node(sync_node);
    const auto target_lanes = stream_sync_target_lanes(graph, sync_event, lane_to_nodes, raw_stream_to_lane, stream_alias_to_lane);
    for (const auto lane : target_lanes) {
        const auto frontier = submit_frontiers.find(lane);
        if (frontier == submit_frontiers.end()) continue;
        const auto submitted_node = find_submitted_frontier_node(frontier->second, sync_event.ts);
        if (submitted_node) graph.add_edge(*submitted_node, sync_node, DagEdgeKind::Sync);
    }
}


} // namespace

void add_event_wait_edges(DagGraph & graph, DagBuildIndex & index) {
    for (const auto record_node : index.event_record_nodes) {
        bind_event_record(graph,
                          record_node,
                          EventRecordBindings{
                              .connection_to_nodes = index.connection_to_nodes,
                              .raw_stream_to_lane = index.raw_stream_to_lane,
                              .stream_alias_to_lane = index.stream_alias_to_lane,
                              .event_id_to_nodes = index.event_id_to_nodes,
                          });
    }
    sort_event_records(graph, index.event_id_to_nodes);

    for (const auto wait_node : index.event_wait_nodes) {
        add_event_wait_dependency(graph,
                                  wait_node,
                                  EventWaitBindings{
                                      .connection_to_nodes = index.connection_to_nodes,
                                      .event_id_to_nodes = index.event_id_to_nodes,
                                  });
    }
}

void add_notify_wait_edges(DagGraph & graph, DagBuildIndex & index) {
    std::ranges::sort(index.notify_record_nodes, [&](size_t a, size_t b) { return graph.event_for_node(a).ts < graph.event_for_node(b).ts; });
    for (size_t wait_node : index.notify_wait_nodes) {
        auto wait_end = node_end_ts(graph.event_for_node(wait_node));
        auto bound_value = wait_end > 200 ? wait_end - 200 : 0;
        auto it = std::upper_bound(index.notify_record_nodes.begin(), index.notify_record_nodes.end(), bound_value, [&](uint64_t value, size_t node_id) {
            return value < graph.event_for_node(node_id).ts;
        });
        if (it == index.notify_record_nodes.begin()) continue;
        --it;
        graph.add_edge(*it, wait_node, DagEdgeKind::Sync);
    }
}

void add_model_execute_edges(DagGraph & graph, DagBuildIndex & index) {
    for (size_t model_node : index.model_execute_nodes) {
        const auto model_start = graph.event_for_node(model_node).ts;
        const auto notify_end = earliest_notify_end(graph, model_start, index.notify_wait_nodes);
        if (notify_end <= model_start) continue;
        const auto model_lane = graph.node(model_node).lane_id;
        for (const auto & [lane_id, nodes] : index.lane_to_nodes) {
            if (nodes.empty() || graph.node(nodes.front()).is_cpu || lane_id == model_lane) continue;
            const auto first_node = first_node_in_window(graph, nodes, model_start, notify_end);
            if (first_node) graph.add_edge(model_node, *first_node, DagEdgeKind::Sync);
        }
    }
}

void add_stream_sync_edges(DagGraph & graph, DagBuildIndex & index) {
    const auto submit_frontiers = build_submit_frontiers(graph, index.lane_to_nodes);
    for (const auto sync_node : index.stream_sync_nodes) {
        add_stream_sync_dependency(graph, sync_node, index.lane_to_nodes, index.raw_stream_to_lane, index.stream_alias_to_lane, submit_frontiers);
    }
}

void add_event_sync_edges(DagGraph & graph, DagBuildIndex & index) {
    for (size_t sync_node : index.event_sync_nodes) {
        const auto & sync_event = graph.event_for_node(sync_node);
        auto event_id = event_id_from_cpu_record(sync_event);
        if (!event_id) {
            const auto connection_id = sync_event.arg("connection_id");
            auto conn_it = index.connection_to_nodes.find(connection_id);
            if (conn_it != index.connection_to_nodes.end() && !conn_it->second.empty())
                event_id = event_id_from_cpu_record(graph.event_for_node(conn_it->second.front()));
        }
        if (!event_id) continue;
        auto records_it = index.event_id_to_nodes.find(*event_id);
        if (records_it == index.event_id_to_nodes.end()) continue;

        auto & records = records_it->second;
        auto bound_value = node_end_ts(sync_event);
        auto bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](uint64_t value, size_t node_id) {
            return value < graph.event_for_node(node_id).ts;
        });
        if (bound == records.begin()) continue;
        --bound;
        graph.add_edge(*bound, sync_node, DagEdgeKind::Sync);
    }
}

void add_device_sync_edges(DagGraph & graph, DagBuildIndex & index) {
    const auto submit_frontiers = build_submit_frontiers(graph, index.lane_to_nodes);
    for (size_t sync_node : index.device_sync_nodes) {
        const auto & sync_event = graph.event_for_node(sync_node);
        for (const auto & item : index.lane_to_nodes) {
            if (item.second.empty() || graph.node(item.second.front()).is_cpu) continue;
            auto frontier_it = submit_frontiers.find(item.first);
            if (frontier_it == submit_frontiers.end()) continue;
            auto submitted_node = find_submitted_frontier_node(frontier_it->second, sync_event.ts);
            if (!submitted_node) continue;
            graph.add_edge(*submitted_node, sync_node, DagEdgeKind::Sync);
        }
    }
}

void finalize_sync_nodes(DagGraph & graph, const DagBuildIndex & index) {
    auto set_fixed_sync_duration = [&](const std::vector<size_t> & nodes) {
        std::ranges::for_each(nodes, [&](size_t node_id) { graph.set_node_duration(node_id, 10); });
    };
    set_fixed_sync_duration(index.stream_sync_nodes);
    set_fixed_sync_duration(index.event_sync_nodes);
    set_fixed_sync_duration(index.device_sync_nodes);
    set_fixed_sync_duration(index.event_wait_nodes);
    set_fixed_sync_duration(index.notify_wait_nodes);
}

} // namespace markov::trace_graph::core
