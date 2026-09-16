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
#include <tuple>
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
using EventRecords = std::unordered_map<std::string, std::vector<DagEventRecord>>;

uint64_t host_start_ns(const TraceEvent & event) { return event.ts * 1000 + event.ts_submicro_ns; }

struct EventRecordBindings {
    const NodeGroups & connection_to_nodes;
    StreamLaneMap & raw_stream_to_lane;
    StreamLaneMap & stream_alias_to_lane;
    EventRecords & event_id_to_records;
};

/** @brief Read-only identity indices used to resolve one event wait. */
struct EventWaitBindings {
    const NodeGroups & connection_to_nodes;
    const EventRecords & event_id_to_records;
};

const TraceEvent * unique_cpu_connection_event(const DagGraph & graph, const std::vector<size_t> & nodes) {
    const TraceEvent * cpu = nullptr;
    for (const auto id : nodes) {
        if (!graph.node(id).is_cpu) continue;
        if (cpu) return nullptr;
        cpu = &graph.event_for_node(id);
    }
    return cpu;
}

void bind_event_record(DagGraph & graph, size_t record_node, const EventRecordBindings & bindings) {
    const auto & record_event = graph.event_for_node(record_node);
    const auto connection = bindings.connection_to_nodes.find(record_event.arg("connection_id"));
    if (connection == bindings.connection_to_nodes.end() || connection->second.empty()) return;
    const auto * cpu_event = unique_cpu_connection_event(graph, connection->second);
    if (!cpu_event) return;
    const auto raw_stream = cpu_event->arg("Raw Stream");
    if (!raw_stream.empty()) {
        const auto lane = graph.node(record_node).lane_id;
        bindings.raw_stream_to_lane[raw_stream] = lane;
        bindings.stream_alias_to_lane[raw_stream] = lane;
    }
    const auto event_id = event_id_from_cpu_record(*cpu_event);
    if (event_id) {
        const auto start = host_start_ns(*cpu_event);
        bindings.event_id_to_records[*event_id].push_back({record_node, start, start + cpu_event->dur * 1000 + cpu_event->dur_submicro_ns});
    }
}

void sort_event_records(EventRecords & event_id_to_records) {
    for (auto & [event_id, records] : event_id_to_records) {
        (void)event_id;
        std::ranges::sort(records, {}, &DagEventRecord::host_start_ns);
        uint64_t prior_end = 0;
        for (auto & record : records) {
            record.ordered_after_prior_calls = prior_end <= record.host_start_ns;
            prior_end = std::max(prior_end, record.host_end_ns);
        }
    }
}

std::optional<size_t> captured_event_record(const std::vector<DagEventRecord> & records, const TraceEvent & host_wait) {
    // Event reuse changes future waits, not a wait already submitted. Device
    // completion timestamps cannot identify which record the host captured.
    const auto start = host_start_ns(host_wait);
    auto bound = std::ranges::upper_bound(records, start, {}, &DagEventRecord::host_start_ns);
    if (bound == records.begin()) return std::nullopt;
    --bound;
    if (!bound->ordered_after_prior_calls || bound->host_end_ns > start || bound->host_start_ns == start) return std::nullopt;
    if (bound != records.begin() && std::prev(bound)->host_start_ns == bound->host_start_ns) return std::nullopt;
    return bound->node_id;
}

void add_event_wait_dependency(DagGraph & graph, size_t wait_node, const EventWaitBindings & bindings) {
    const auto & wait = graph.event_for_node(wait_node);
    const auto connection = bindings.connection_to_nodes.find(wait.arg("connection_id"));
    if (connection == bindings.connection_to_nodes.end()) return;
    const auto * cpu = unique_cpu_connection_event(graph, connection->second);
    if (!cpu) return;
    const auto event_id = event_id_from_cpu_record(*cpu);
    if (!event_id) return;
    const auto records = bindings.event_id_to_records.find(*event_id);
    if (records == bindings.event_id_to_records.end()) return;
    const auto record = captured_event_record(records->second, *cpu);
    if (record) graph.add_edge(*record, wait_node, DagEdgeKind::Sync);
}

struct StreamSubmission {
    size_t position = 0;
    uint64_t start_ns = 0, end_ns = 0;
};

struct StreamWaitPlacement {
    size_t lane = 0, position = 0, host = 0, record = 0;
    uint64_t start_ns = 0;
};

void add_unrecorded_stream_waits(DagGraph & graph, DagBuildIndex & index) {
    // A runtime may omit a device WAIT when its event is already complete.
    // Keep the program dependency: changing producer cost can make it block.
    if (index.native_stream_wait_nodes.empty()) return;
    StreamLaneMap streams;
    std::unordered_map<size_t, std::vector<StreamSubmission>> submissions;
    for (const auto & [lane, nodes] : index.lane_to_nodes) {
        if (nodes.empty() || graph.node(nodes.front()).is_cpu) continue;
        auto & ordered = submissions[lane];
        for (size_t position = 0; position < nodes.size(); ++position) {
            const auto & device = graph.event_for_node(nodes[position]);
            const auto connection = index.connection_to_nodes.find(device.arg("connection_id"));
            if (connection == index.connection_to_nodes.end()) continue;
            const auto * host = unique_cpu_connection_event(graph, connection->second);
            if (!host) continue;
            const auto start = host_start_ns(*host);
            ordered.push_back({position, start, start + host->dur * 1000 + host->dur_submicro_ns});
            if (const auto raw = host->arg("Raw Stream"); !raw.empty()) {
                const auto [found, inserted] = streams.emplace(raw, lane);
                if (!inserted && found->second != lane) found->second = DagNode::kNoNode;
            }
        }
        // A reordered or partially unknown host stream cannot identify a
        // unique before/after boundary merely from device timestamps.
        if (!std::ranges::is_sorted(ordered, {}, &StreamSubmission::start_ns)) ordered.clear();
    }
    for (const auto & [raw, lane] : streams) {
        if (lane == DagNode::kNoNode) {
            index.raw_stream_to_lane.erase(raw);
            index.stream_alias_to_lane.erase(raw);
        } else {
            index.raw_stream_to_lane[raw] = lane;
            index.stream_alias_to_lane[raw] = lane;
        }
    }

    std::vector<StreamWaitPlacement> pending;
    for (const auto host_id : index.native_stream_wait_nodes) {
        const auto & host = graph.event_for_node(host_id);
        const auto status = [&](const char * value) { graph.mutable_event_for_node(host_id).set_arg("stream_wait_binding", value); };
        const auto connection = index.connection_to_nodes.find(host.arg("connection_id"));
        if (connection != index.connection_to_nodes.end() && std::ranges::any_of(connection->second,
            [&](size_t id) { return !graph.node(id).is_cpu; })) { status("observed_device"); continue; }
        const auto event_id = event_id_from_cpu_record(host);
        const auto records = event_id ? index.event_id_to_records.find(*event_id) : index.event_id_to_records.end();
        const auto record = records == index.event_id_to_records.end() ? std::nullopt : captured_event_record(records->second, host);
        if (!record) { status("missing_record"); continue; }
        const auto stream = streams.find(host.arg("Raw Stream"));
        if (stream == streams.end() || stream->second == DagNode::kNoNode) { status("missing_stream"); continue; }
        const auto & ordered = submissions.at(stream->second);
        if (ordered.empty()) { status("unordered_submissions"); continue; }
        const auto start = host_start_ns(host), end = start + host.dur * 1000 + host.dur_submicro_ns;
        const auto next = std::ranges::lower_bound(ordered, end, {}, &StreamSubmission::start_ns);
        const auto & nodes = index.lane_to_nodes.at(stream->second);
        const auto position = next == ordered.end() ? nodes.size() : next->position;
        if ((next == ordered.begin() && position != 0)
            || (next != ordered.begin() && (std::prev(next)->position + 1 != position || std::prev(next)->end_ns > start))) {
            status("ambiguous_boundary"); continue;
        }
        pending.push_back({stream->second, position, host_id, *record, start});
    }

    std::ranges::sort(pending, [](const auto & a, const auto & b) {
        return std::tie(a.lane, a.position, a.start_ns, a.host) < std::tie(b.lane, b.position, b.start_ns, b.host);
    });
    graph.reserve({.nodes = graph.node_count() + pending.size(), .edges = graph.edges().size() + 4 * pending.size()});
    graph.mutable_events().reserve(graph.events().size() + pending.size());
    for (size_t begin = 0; begin < pending.size();) {
        size_t stop = begin + 1;
        while (stop < pending.size() && pending[stop].lane == pending[begin].lane) ++stop;
        auto & nodes = index.lane_to_nodes.at(pending[begin].lane);
        std::vector<size_t> ordered;
        ordered.reserve(nodes.size() + stop - begin);
        size_t wait = begin;
        for (size_t position = 0; position <= nodes.size(); ++position) {
            while (wait < stop && pending[wait].position == position) {
                const auto & placement = pending[wait++];
                const auto host = graph.event_for_node(placement.host);
                const auto id = graph.add_synthetic_node({.name = "logical_event_wait", .category = "dependency",
                    .is_cpu = false, .lane_key = std::string(graph.lane_key(placement.lane)),
                    .attrs = {{"Event Id", host.arg("Event Id")}, {"Raw Stream", host.arg("Raw Stream")}}});
                graph.mutable_event_for_node(id).ts = host.ts;
                graph.mutable_event_for_node(id).ts_submicro_ns = host.ts_submicro_ns;
                graph.mutable_node(id).submit_ts = host.ts;
                graph.add_edge(placement.host, id, DagEdgeKind::Correlation);
                graph.add_edge(placement.record, id, DagEdgeKind::Sync);
                graph.mutable_event_for_node(placement.host).set_arg("stream_wait_binding", "logical");
                ordered.push_back(id);
            }
            if (position < nodes.size()) ordered.push_back(nodes[position]);
        }
        for (size_t i = 1; i < ordered.size(); ++i) {
            if (graph.node(ordered[i - 1]).kind == DagNodeKind::Synthetic || graph.node(ordered[i]).kind == DagNodeKind::Synthetic)
                graph.add_edge(ordered[i - 1], ordered[i], DagEdgeKind::Stream);
        }
        nodes = std::move(ordered);
        begin = stop;
    }
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
                              .event_id_to_records = index.event_id_to_records,
                          });
    }
    sort_event_records(index.event_id_to_records);

    for (const auto wait_node : index.event_wait_nodes) {
        add_event_wait_dependency(graph,
                                  wait_node,
                                  EventWaitBindings{
                                      .connection_to_nodes = index.connection_to_nodes,
                                      .event_id_to_records = index.event_id_to_records,
                                  });
    }
    add_unrecorded_stream_waits(graph, index);
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
            if (conn_it != index.connection_to_nodes.end()) {
                if (const auto * cpu = unique_cpu_connection_event(graph, conn_it->second)) event_id = event_id_from_cpu_record(*cpu);
            }
        }
        if (!event_id) continue;
        auto records_it = index.event_id_to_records.find(*event_id);
        if (records_it == index.event_id_to_records.end()) continue;
        const auto record = captured_event_record(records_it->second, sync_event);
        if (record) graph.add_edge(*record, sync_node, DagEdgeKind::Sync);
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
    std::vector<bool> has_wait_dependency(graph.node_count(), false);
    for (const auto & edge : graph.edges()) {
        if (edge.active && edge.kind == DagEdgeKind::Sync) has_wait_dependency[edge.dst] = true;
    }
    auto set_fixed_sync_duration = [&](const std::vector<size_t> & nodes) {
        for (const auto node_id : nodes) {
            // A submission or stream-order edge alone does not replace a wait.
            // Keep the observed blocking interval when its object is unknown.
            if (has_wait_dependency[node_id]) graph.set_node_duration(node_id, 10);
        }
    };
    set_fixed_sync_duration(index.stream_sync_nodes);
    set_fixed_sync_duration(index.event_sync_nodes);
    set_fixed_sync_duration(index.device_sync_nodes);
    set_fixed_sync_duration(index.event_wait_nodes);
    set_fixed_sync_duration(index.notify_wait_nodes);
}

} // namespace markov::trace_graph::core
