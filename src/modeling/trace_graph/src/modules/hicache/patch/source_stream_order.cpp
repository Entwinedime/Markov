#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::patch {

std::optional<size_t> HiCacheDeviceStreamOrder::previous(size_t node) const {
    const auto position = positions.at(node);
    return position ? std::optional<size_t>{ nodes[position - 1] } : std::nullopt;
}

std::optional<size_t> HiCacheDeviceStreamOrder::next(size_t node) const {
    const auto position = positions.at(node) + 1;
    return position < nodes.size() ? std::optional<size_t>{ nodes[position] } : std::nullopt;
}

const HiCacheDeviceStreamOrder & HiCacheSourceDagIndex::device_stream_order(size_t sample) const {
    const auto & selected = graph_.node(sample);
    // The sample identifies a physical resource, not a dependency endpoint.
    // A replaced call may remove it while its rank/lane remains usable.
    if (selected.is_cpu) throw std::invalid_argument("Device stream order requires a device resource sample");
    const auto key = std::pair{ selected.gpu_id, selected.lane_id };
    if (const auto found = device_stream_orders_.find(key); found != device_stream_orders_.end()) return found->second;
    std::unordered_map<size_t, size_t> indegrees;
    for (const auto & node : graph_.nodes())
        if (node.active && !node.is_cpu && node.gpu_id == key.first && node.lane_id == key.second) indegrees.emplace(node.id, 0);
    for (auto & [node, count] : indegrees)
        for (const auto id : incoming_edge_ids(node)) {
            const auto & edge = graph_.edge(id);
            if (!edge.active || edge.kind != core::DagEdgeKind::Stream || !graph_.node(edge.src).active) continue;
            if (!indegrees.contains(edge.src)) throw std::runtime_error("Stream dependency crosses its physical rank/lane");
            ++count;
        }
    std::vector<size_t> ready;
    for (const auto & [node, count] : indegrees)
        if (!count) ready.push_back(node);
    HiCacheDeviceStreamOrder order;
    order.nodes.reserve(indegrees.size());
    while (!ready.empty()) {
        if (ready.size() != 1) throw std::runtime_error("Device stream has no unique dependency order");
        const auto node = ready.back();
        ready.pop_back();
        order.positions.emplace(node, order.nodes.size());
        order.nodes.push_back(node);
        for (const auto id : outgoing_edge_ids(node)) {
            const auto & edge = graph_.edge(id);
            if (!edge.active || edge.kind != core::DagEdgeKind::Stream || !graph_.node(edge.dst).active) continue;
            const auto next = indegrees.find(edge.dst);
            if (next == indegrees.end()) throw std::runtime_error("Stream dependency crosses its physical rank/lane");
            if (--next->second == 0) ready.push_back(edge.dst);
        }
    }
    if (order.nodes.size() != indegrees.size()) throw std::runtime_error("Device stream contains a dependency cycle");
    return device_stream_orders_.emplace(key, std::move(order)).first->second;
}

} // namespace markov::trace_graph::modules::hicache::patch
