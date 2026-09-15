/**
 * @file
 * @brief Orders execution lanes and separates CPU queue service from arrival waits.
 */
#include "dag_builder_stages.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <tuple>
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

bool contains_any_hccl_name(const std::string & name) { return name.contains("hcom") || name.contains("HCCL") || name.contains("hccl"); }

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
