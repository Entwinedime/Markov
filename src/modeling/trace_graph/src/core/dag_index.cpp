#include "core/dag_index.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::core {

std::shared_ptr<const DagAdjacency> DagGraph::adjacency() const {
    if (adjacency_) return adjacency_;

    DagAdjacency index;
    index.incoming_offsets.assign(node_count() + 1, 0);
    index.outgoing_offsets.assign(node_count() + 1, 0);

    for (const auto & edge : edges()) {
        if (!edge.active) continue;
        if (edge.src >= node_count() || edge.dst >= node_count()) throw std::logic_error("Source DAG index found an edge with an invalid endpoint");
        if (!node(edge.src).active || !node(edge.dst).active) throw std::logic_error("Source DAG index found an active edge attached to an inactive node");
        ++index.outgoing_offsets[edge.src + 1];
        ++index.incoming_offsets[edge.dst + 1];
    }
    for (size_t node_id = 0; node_id < node_count(); ++node_id) {
        index.outgoing_offsets[node_id + 1] += index.outgoing_offsets[node_id];
        index.incoming_offsets[node_id + 1] += index.incoming_offsets[node_id];
    }
    index.outgoing_edge_ids.resize(index.outgoing_offsets.back());
    index.incoming_edge_ids.resize(index.incoming_offsets.back());
    auto outgoing_cursor = index.outgoing_offsets;
    auto incoming_cursor = index.incoming_offsets;
    for (size_t edge_index = 0; edge_index < edge_count(); ++edge_index) {
        const auto & edge = this->edge(edge_index);
        if (!edge.active) continue;
        index.outgoing_edge_ids[outgoing_cursor[edge.src]++] = edge_index;
        index.incoming_edge_ids[incoming_cursor[edge.dst]++] = edge_index;
    }

    return adjacency_ = std::make_shared<const DagAdjacency>(std::move(index));
}

std::shared_ptr<const CpuIntervalIndex> DagGraph::cpu_intervals() const {
    if (cpu_intervals_) return cpu_intervals_;

    CpuIntervalIndex index;
    index.node_count = node_count();
    index.event_count = event_count();
    if (source_cpu_intervals_) {
        // Activity narrows the source order; raw-coordinate edits invalidate this seed.
        for (const auto & [lane, members] : source_cpu_intervals_->nodes) {
            auto & retained = index.nodes[lane];
            retained.reserve(members.size());
            for (const auto id : members)
                if (node(id).active) retained.push_back(id);
        }
    }
    const auto first_added = source_cpu_intervals_ ? source_cpu_intervals_->node_count : 0;
    std::unordered_map<std::string, size_t, TraceArgHash, std::equal_to<>> retained_counts;
    for (const auto & [lane, members] : index.nodes) retained_counts.emplace(lane, members.size());
    for (size_t id = first_added; id < node_count(); ++id) {
        const auto & node = this->node(id);
        if (!node.active || !node.is_cpu) continue;

        const auto & event = event_for_node(node.id);
        index.nodes[CpuIntervalIndex::lane_key(event.pid, event.tid)].push_back(node.id);
    }

    for (auto & [lane_key, nodes] : index.nodes) {
        // Sort compact coordinates instead of repeatedly visiting large trace
        // storage. Entry points still precede nonempty leaves at the same tick.
        const auto retained = retained_counts.contains(lane_key) ? retained_counts.at(lane_key) : 0;
        if (retained < nodes.size()) {
            const auto coordinates = [&](size_t id) {
                const auto & event = event_for_node(id);
                return std::tuple{ event.ts, event.dur != 0, id };
            };
            std::vector<std::tuple<uint64_t, bool, size_t>> order;
            order.reserve(nodes.size() - retained);
            for (size_t i = retained; i < nodes.size(); ++i) order.push_back(coordinates(nodes[i]));
            if (!std::ranges::is_sorted(order)) std::ranges::sort(order);
            for (size_t i = 0; i < order.size(); ++i) nodes[retained + i] = std::get<2>(order[i]);
            if (retained) std::ranges::inplace_merge(nodes, nodes.begin() + retained, std::ranges::less{}, coordinates);
        }

        auto & prefix_ends = index.prefix_end_us[lane_key];
        prefix_ends.reserve(nodes.size());
        uint64_t frontier_end = 0;
        for (size_t node_id : nodes) {
            const auto & event = event_for_node(node_id);
            frontier_end =
                std::max(frontier_end,
                         (event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur));
            prefix_ends.push_back(frontier_end);
        }
    }

    cpu_intervals_ = std::make_shared<const CpuIntervalIndex>(std::move(index));
    if (!source_cpu_intervals_) source_cpu_intervals_ = cpu_intervals_;
    return cpu_intervals_;
}

std::shared_ptr<const CpuTaskObservations> DagGraph::cpu_task_observations() const {
    if (cpu_task_observations_) return cpu_task_observations_;

    CpuTaskObservations index;
    index.node_count = node_count();
    index.edge_count = edge_count();
    index.event_count = event_count();
    for (const auto & node : nodes()) {
        if (!node.is_cpu || node.explicit_cpu_task) continue;

        const auto & event = event_for_node(node.id);
        const auto correlation = event.arg("correlation_id");
        size_t identity = DagNode::kNoNode;
        if (!correlation.empty()) {
            identity = index.identities.try_emplace({ node.gpu_id, event.pid, correlation }, index.identities.size()).first->second;
            if (event.cat == "enqueue") index.submissions.emplace_back(identity, node.id);
        }
        index.lanes[node.lane_id].push_back({ node.id, identity, event.ts, checked_add_u64(event.ts, event.dur, "CPU task observation end overflow") });
    }
    const auto before = [](const auto & left, const auto & right) { return std::tie(left.start_us, left.node) < std::tie(right.start_us, right.node); };
    for (auto & [lane, members] : index.lanes)
        if (!std::ranges::is_sorted(members, before)) std::ranges::sort(members, before);

    // Endpoints may later move; retain every edge of these kinds, not just current cross-lane edges.
    for (size_t id = 0; id < edge_count(); ++id)
        if (edge(id).kind == DagEdgeKind::Sequential || edge(id).kind == DagEdgeKind::Correlation) index.task_edges.push_back(id);

    return cpu_task_observations_ = std::make_shared<const CpuTaskObservations>(std::move(index));
}

} // namespace markov::trace_graph::core
