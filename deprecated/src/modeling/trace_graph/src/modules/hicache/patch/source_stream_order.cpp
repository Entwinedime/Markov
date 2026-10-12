#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include <set>
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

const std::vector<std::vector<size_t>> & HiCacheSourceDagIndex::device_submission_anchors(size_t sample, const std::string & pid,
                                                                                          const std::string & tid) const {
    const auto & resource = graph_.node(sample);
    const auto key = std::tuple{ resource.gpu_id, resource.lane_id, pid, tid };
    if (const auto found = device_submission_anchors_.find(key); found != device_submission_anchors_.end()) return found->second;

    std::vector<std::vector<size_t>> result;
    for (const auto current : device_stream_order(sample).nodes) {
        std::set<size_t> anchors, searched;
        std::vector<size_t> pending{ current };
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            if (!searched.insert(node).second) continue;
            const auto & event = graph_.event_for_node(node);
            if (graph_.node(node).is_cpu && event.pid == pid && event.tid == tid) {
                anchors.insert(node);
                continue;
            }
            for (const auto id : incoming_edge_ids(node)) {
                const auto & edge = graph_.edge(id);
                if (edge.active && edge.kind == core::DagEdgeKind::Correlation && graph_.node(edge.src).active) pending.push_back(edge.src);
            }
        }
        result.emplace_back(anchors.begin(), anchors.end());
    }
    return device_submission_anchors_.emplace(key, std::move(result)).first->second;
}

bool HiCacheSourceDagIndex::device_submitted_in_call(size_t node, const std::string & pid, const std::string & tid, uint64_t begin_us, uint64_t end_us) const {
    const auto & order = device_stream_order(node);
    const auto & anchors = device_submission_anchors(node, pid, tid).at(order.positions.at(node));
    if (anchors.empty()) return false;

    for (const auto anchor : anchors) {
        const auto & event = graph_.event_for_node(anchor);
        if (event.ts < begin_us || event.ts >= end_us || event.dur > end_us - event.ts) return false;
    }
    return true;
}

} // namespace markov::trace_graph::modules::hicache::patch
