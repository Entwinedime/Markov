/**
 * @file
 * @brief Orders execution lanes and proven request lifecycle boundaries.
 */
#include "dag_builder_stages.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace markov::trace_graph::core {

void sort_nodes_by_event_ts_if_needed(const DagGraph & graph, std::vector<size_t> & nodes) {
    const auto earlier = [&](size_t a, size_t b) {
        const auto & left = graph.event_for_node(a);
        const auto & right = graph.event_for_node(b);
        // CPU self-time fragments are partitioned in whole microseconds and
        // share that stable order with scope indexing. Only device observations
        // retain fractional boundaries throughout normalization.
        const auto left_fraction = graph.node(a).is_cpu ? 0 : left.ts_submicro_ns;
        const auto right_fraction = graph.node(b).is_cpu ? 0 : right.ts_submicro_ns;
        return std::tie(left.ts, left_fraction, a) < std::tie(right.ts, right_fraction, b);
    };
    if (!std::ranges::is_sorted(nodes, earlier)) std::ranges::sort(nodes, earlier);
}

namespace {

using dag_builder_detail::node_end_ts;

using LaneNodes = std::unordered_map<size_t, std::vector<size_t>>;

bool contains_any_hccl_name(const std::string & name) { return name.contains("hcom") || name.contains("HCCL") || name.contains("hccl"); }

struct RequestLifecycleBoundary {
    std::string request_id;
    uint64_t lookup_ts = 0;
    uint64_t commit_end_ts = 0;
};

struct LaneOrderBuffers {
    std::vector<size_t> & nodes;
    std::vector<size_t> & notify_wait_nodes;
};

void record_lane_edge_metadata(DagGraph & graph, size_t previous_node, size_t node_id, bool is_cpu, std::vector<size_t> & notify_wait_nodes) {
    const auto & previous = graph.event_for_node(previous_node);
    const auto & event = graph.event_for_node(node_id);
    if (is_cpu) {
        const auto previous_end = node_end_ts(previous);
        auto & previous_dag_node = graph.mutable_node(previous_node);
        previous_dag_node.cpu_gap_after = event.ts > previous_end ? event.ts - previous_end : 0;
        previous_dag_node.original_cpu_gap_after = previous_dag_node.cpu_gap_after;
    }
    if (event.name == "NOTIFY_WAIT" && previous.name == "MODEL_EXECUTE") notify_wait_nodes.push_back(node_id);
    if (!is_cpu && contains_any_hccl_name(previous.name)) graph.mutable_node(previous_node).hccl_successor_node_id = node_id;
}

void add_lane_order_edges(DagGraph & graph, const LaneOrderBuffers & buffers) {
    sort_nodes_by_event_ts_if_needed(graph, buffers.nodes);
    if (buffers.nodes.empty()) return;
    const bool is_cpu = graph.node(buffers.nodes.front()).is_cpu;
    const auto edge_kind = is_cpu ? DagEdgeKind::Sequential : DagEdgeKind::Stream;
    for (size_t index = 1; index < buffers.nodes.size(); ++index) {
        const auto previous_node = buffers.nodes[index - 1];
        const auto node_id = buffers.nodes[index];
        graph.add_edge(previous_node, node_id, edge_kind);
        record_lane_edge_metadata(graph, previous_node, node_id, is_cpu, buffers.notify_wait_nodes);
    }
}

std::vector<RequestLifecycleBoundary> request_lifecycle_boundaries(const DagGraph & graph) {
    std::unordered_map<std::string, RequestLifecycleBoundary> by_request;
    for (const auto & event : graph.hicache_fact_events()) {
        if (event.arg("fact.class") != "workload_identity") continue;
        const auto request_id = event.arg("request_id");
        if (request_id.empty()) continue;
        const auto role = event.arg("fact.role");
        auto & boundary = by_request[request_id];
        boundary.request_id = request_id;
        if (role == "cache_lookup_input" && (boundary.lookup_ts == 0 || event.ts < boundary.lookup_ts)) boundary.lookup_ts = event.ts;
        if (role == "cache_lifecycle_commit" && event.arg("lifecycle_kind") == "finished") {
            boundary.commit_end_ts = std::max(boundary.commit_end_ts, node_end_ts(event));
        }
    }

    std::vector<RequestLifecycleBoundary> boundaries;
    boundaries.reserve(by_request.size());
    for (auto & [request_id, boundary] : by_request) {
        (void)request_id;
        if (boundary.lookup_ts > 0) boundaries.push_back(std::move(boundary));
    }
    std::ranges::sort(boundaries, [](const auto & left, const auto & right) {
        if (left.lookup_ts != right.lookup_ts) return left.lookup_ts < right.lookup_ts;
        return left.request_id < right.request_id;
    });
    return boundaries;
}

std::optional<size_t> first_cpu_node_at_or_after(const DagGraph & graph, const LaneNodes & lane_to_nodes, uint64_t timestamp) {
    std::optional<size_t> selected;
    for (const auto & [lane_id, nodes] : lane_to_nodes) {
        (void)lane_id;
        if (nodes.empty() || !graph.node(nodes.front()).is_cpu) continue;
        auto node =
            std::lower_bound(nodes.begin(), nodes.end(), timestamp, [&](size_t node_id, uint64_t value) { return graph.event_for_node(node_id).ts < value; });
        while (node != nodes.end() && graph.event_for_node(*node).name.ends_with(".self")) ++node;
        if (node == nodes.end()) continue;
        if (!selected || graph.event_for_node(*node).ts < graph.event_for_node(*selected).ts) selected = *node;
    }
    return selected;
}

std::optional<size_t> device_frontier_before(const DagGraph & graph, const std::vector<size_t> & nodes, uint64_t timestamp) {
    if (nodes.empty() || graph.node(nodes.front()).is_cpu) return std::nullopt;
    auto node =
        std::lower_bound(nodes.begin(), nodes.end(), timestamp, [&](size_t node_id, uint64_t value) { return graph.event_for_node(node_id).ts < value; });
    if (node == nodes.begin()) return std::nullopt;
    --node;
    return *node;
}

std::optional<size_t> first_device_node_at_or_after(const DagGraph & graph, const std::vector<size_t> & nodes, uint64_t timestamp) {
    if (nodes.empty() || graph.node(nodes.front()).is_cpu) return std::nullopt;
    const auto node =
        std::lower_bound(nodes.begin(), nodes.end(), timestamp, [&](size_t node_id, uint64_t value) { return graph.event_for_node(node_id).ts < value; });
    if (node == nodes.end()) return std::nullopt;
    return *node;
}

void add_request_boundary_dependencies(DagGraph & graph, const LaneNodes & lane_to_nodes) {
    const auto boundaries = request_lifecycle_boundaries(graph);
    for (size_t index = 1; index < boundaries.size(); ++index) {
        const auto & previous = boundaries[index - 1];
        const auto & current = boundaries[index];
        if (previous.commit_end_ts == 0 || previous.commit_end_ts >= current.lookup_ts) continue;
        const auto cpu_anchor = first_cpu_node_at_or_after(graph, lane_to_nodes, current.lookup_ts);
        if (!cpu_anchor) continue;
        for (const auto & [lane_id, nodes] : lane_to_nodes) {
            (void)lane_id;
            const auto frontier = device_frontier_before(graph, nodes, current.lookup_ts);
            if (frontier) graph.add_edge(*frontier, *cpu_anchor, DagEdgeKind::Sync);
        }
        for (const auto & [lane_id, nodes] : lane_to_nodes) {
            (void)lane_id;
            const auto first = first_device_node_at_or_after(graph, nodes, current.lookup_ts);
            if (first) graph.add_edge(*cpu_anchor, *first, DagEdgeKind::Sync);
        }
    }
}


} // namespace

void add_sequential_edges(DagGraph & graph, DagBuildIndex & index) {
    /**
     * @brief Orders each execution lane by event timestamp.
     *
     * CPU lanes produce sequential edges and device lanes produce stream edges. Both
     * are hard dependencies; distinct kinds preserve diagnostic attribution.
     */
    for (auto & [lane_id, nodes] : index.lane_to_nodes) {
        (void)lane_id;
        add_lane_order_edges(graph, LaneOrderBuffers{ .nodes = nodes, .notify_wait_nodes = index.notify_wait_nodes });
    }
}

void add_request_boundary_edges(DagGraph & graph, DagBuildIndex & index) { add_request_boundary_dependencies(graph, index.lane_to_nodes); }

void normalize_cpu_queue_waits(DagGraph & graph) {
    struct QueuePredecessors {
        size_t worker = DagNode::kNoNode;
        size_t submission = DagNode::kNoNode;
        size_t count = 0;
    };
    std::vector<QueuePredecessors> predecessors(graph.node_count());
    for (const auto & edge : graph.edges()) {
        if (!edge.active) continue;
        auto & before = predecessors[edge.dst];
        ++before.count;
        const auto & next = graph.event_for_node(edge.dst);
        if (!graph.node(edge.dst).is_cpu) continue;
        const auto & source = graph.node(edge.src);
        if (edge.kind == DagEdgeKind::Sequential && source.lane_id == graph.node(edge.dst).lane_id)
            before.worker = edge.src;
        if (edge.kind != DagEdgeKind::Correlation || !source.is_cpu || source.lane_id == graph.node(edge.dst).lane_id) continue;
        const auto & submit = graph.event_for_node(edge.src);
        if (submit.cat == "enqueue" && submit.pid == next.pid && !submit.arg("correlation_id").empty()
            && submit.arg("correlation_id") == next.arg("correlation_id")) before.submission = edge.src;
    }
    for (size_t id = 0; id < predecessors.size(); ++id) {
        const auto & before = predecessors[id];
        // Only a proven two-input queue join: ambiguous or additional dependencies
        // need their own explanation, and keep their original timing here.
        if (before.count != 2 || before.worker == DagNode::kNoNode || before.submission == DagNode::kNoNode) continue;
        const auto ready = std::max(node_end_ts(graph.event_for_node(before.worker)), node_end_ts(graph.event_for_node(before.submission)));
        const auto start = graph.event_for_node(id).ts;
        if (ready > start) continue;
        graph.mutable_node(before.worker).cpu_gap_after = 0;
        graph.mutable_node(id).cpu_ready_delay_before = start - ready;
    }
}


} // namespace markov::trace_graph::core
