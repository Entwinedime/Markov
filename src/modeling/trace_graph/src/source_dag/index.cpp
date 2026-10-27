#include "markov/trace_graph/source_dag/index.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::source_dag {
namespace index_detail {

uint64_t saturated_add(uint64_t start, uint64_t duration) {
    return duration > std::numeric_limits<uint64_t>::max() - start ? std::numeric_limits<uint64_t>::max() : start + duration;
}

} // namespace index_detail

SourceDagIndex::SourceDagIndex(const core::DagGraph & graph) : graph_(graph) {}

const SourceDagIndex::Adjacency & SourceDagIndex::adjacency() const {
    if (adjacency_) return *adjacency_;

    Adjacency index;
    index.incoming_offsets.assign(graph_.node_count() + 1, 0);
    index.outgoing_offsets.assign(graph_.node_count() + 1, 0);

    for (const auto & edge : graph_.edges()) {
        if (!edge.active) continue;
        if (edge.src >= graph_.node_count() || edge.dst >= graph_.node_count())
            throw std::logic_error("Source DAG index found an edge with an invalid endpoint");
        if (!graph_.node(edge.src).active || !graph_.node(edge.dst).active)
            throw std::logic_error("Source DAG index found an active edge attached to an inactive node");
        ++index.outgoing_offsets[edge.src + 1];
        ++index.incoming_offsets[edge.dst + 1];
    }
    for (size_t node_id = 0; node_id < graph_.node_count(); ++node_id) {
        index.outgoing_offsets[node_id + 1] += index.outgoing_offsets[node_id];
        index.incoming_offsets[node_id + 1] += index.incoming_offsets[node_id];
    }
    index.outgoing_edge_ids.resize(index.outgoing_offsets.back());
    index.incoming_edge_ids.resize(index.incoming_offsets.back());
    auto outgoing_cursor = index.outgoing_offsets;
    auto incoming_cursor = index.incoming_offsets;
    for (size_t edge_index = 0; edge_index < graph_.edge_count(); ++edge_index) {
        const auto & edge = graph_.edge(edge_index);
        if (!edge.active) continue;
        index.outgoing_edge_ids[outgoing_cursor[edge.src]++] = edge_index;
        index.incoming_edge_ids[incoming_cursor[edge.dst]++] = edge_index;
    }

    return adjacency_.emplace(std::move(index));
}

const SourceDagIndex::CpuIndex & SourceDagIndex::cpu_index() const {
    if (cpu_index_) return *cpu_index_;

    CpuIndex index;
    for (const auto & node : graph_.nodes()) {
        if (!node.active || !node.is_cpu) continue;

        const auto & event = graph_.event_for_node(node.id);
        index.nodes[cpu_lane_key(event.pid, event.tid)].push_back(node.id);
    }

    for (auto & [lane_key, nodes] : index.nodes) {
        // Sort compact coordinates instead of repeatedly visiting large trace
        // storage. Entry points still precede nonempty leaves at the same tick.
        std::vector<std::tuple<uint64_t, bool, size_t>> order;
        order.reserve(nodes.size());
        for (const auto id : nodes) {
            const auto & event = graph_.event_for_node(id);
            order.emplace_back(event.ts, event.dur != 0, id);
        }
        std::ranges::sort(order);
        for (size_t i = 0; i < nodes.size(); ++i) nodes[i] = std::get<2>(order[i]);

        auto & prefix_ends = index.prefix_end_us[lane_key];
        prefix_ends.reserve(nodes.size());
        uint64_t frontier_end = 0;
        for (size_t node_id : nodes) {
            const auto & event = graph_.event_for_node(node_id);
            frontier_end = std::max(frontier_end, index_detail::saturated_add(event.ts, event.dur));
            prefix_ends.push_back(frontier_end);
        }
    }

    return cpu_index_.emplace(std::move(index));
}

std::span<const size_t> SourceDagIndex::incoming_edge_ids(size_t node_id) const {
    if (node_id >= graph_.node_count()) throw std::out_of_range("Source DAG incoming adjacency node ID is out of range");
    const auto & index = adjacency();
    return std::span<const size_t>{ index.incoming_edge_ids }.subspan(index.incoming_offsets[node_id],
                                                                      index.incoming_offsets[node_id + 1] - index.incoming_offsets[node_id]);
}

std::span<const size_t> SourceDagIndex::outgoing_edge_ids(size_t node_id) const {
    if (node_id >= graph_.node_count()) throw std::out_of_range("Source DAG outgoing adjacency node ID is out of range");
    const auto & index = adjacency();
    return std::span<const size_t>{ index.outgoing_edge_ids }.subspan(index.outgoing_offsets[node_id],
                                                                      index.outgoing_offsets[node_id + 1] - index.outgoing_offsets[node_id]);
}

std::span<const size_t> SourceDagIndex::find_nodes(const NodeMap & index, std::string_view key) {
    const auto it = index.find(key);
    return it == index.end() ? std::span<const size_t>{} : std::span<const size_t>{ it->second };
}

std::span<const size_t> SourceDagIndex::cpu_nodes_on_lane(std::string_view pid, std::string_view tid) const {
    return find_nodes(cpu_index().nodes, cpu_lane_key(pid, tid));
}

std::span<const size_t> SourceDagIndex::cpu_interval_candidates(std::string_view pid, std::string_view tid, uint64_t begin_us, uint64_t end_us) const {
    const auto key = cpu_lane_key(pid, tid);
    const auto & cpu = cpu_index();
    const auto nodes = find_nodes(cpu.nodes, key);
    if (nodes.empty() || end_us <= begin_us) return {};

    const auto & ends = cpu.prefix_end_us.at(key);
    const auto first = static_cast<size_t>(std::ranges::upper_bound(ends, begin_us) - ends.begin());
    const auto last = static_cast<size_t>(std::ranges::lower_bound(nodes, end_us, {}, [&](size_t id) { return graph_.event_for_node(id).ts; }) - nodes.begin());
    return nodes.subspan(first, last - first);
}

std::string SourceDagIndex::cpu_lane_key(std::string_view pid, std::string_view tid) { return std::string(pid) + "\x1f" + std::string(tid); }

std::optional<size_t> SourceDagIndex::cpu_node_starting_at(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const {
    const auto nodes = cpu_nodes_on_lane(pid, tid);
    const auto first = std::ranges::lower_bound(nodes, timestamp_us, {}, [&](size_t id) { return graph_.event_for_node(id).ts; });
    if (first == nodes.end() || graph_.event_for_node(*first).ts != timestamp_us) return std::nullopt;
    const auto next = std::next(first);
    if (next != nodes.end() && graph_.event_for_node(*next).ts == timestamp_us) return std::nullopt;
    return *first;
}

std::optional<size_t> SourceDagIndex::cpu_boundary_at_or_before(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const {
    const auto nodes = cpu_nodes_on_lane(pid, tid);
    const auto bound = std::upper_bound(nodes.begin(), nodes.end(), timestamp_us, [&](uint64_t timestamp, size_t node_id) {
        return timestamp < graph_.event_for_node(node_id).ts;
    });
    if (bound == nodes.begin()) return std::nullopt;
    return *(bound - 1);
}

std::optional<size_t> SourceDagIndex::cpu_boundary_at_or_after(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const {
    const auto nodes = cpu_nodes_on_lane(pid, tid);
    const auto bound = std::lower_bound(nodes.begin(), nodes.end(), timestamp_us, [&](size_t node_id, uint64_t timestamp) {
        return graph_.event_for_node(node_id).ts < timestamp;
    });
    if (bound != nodes.begin()) {
        const auto previous = *(bound - 1);
        const auto & event = graph_.event_for_node(previous);
        const auto event_end = index_detail::saturated_add(event.ts, event.dur);
        if (timestamp_us <= event_end) return previous;
    }
    return bound == nodes.end() ? std::nullopt : std::optional<size_t>{ *bound };
}

TimingIntervalOwnership SourceDagIndex::timing_interval_ownership(std::string_view pid, std::string_view tid, uint64_t start_us, uint64_t duration_us) const {
    TimingIntervalOwnership output{
        .interval_start_us = start_us,
        .interval_end_us = index_detail::saturated_add(start_us, duration_us),
        .observed_duration_us = duration_us,
    };
    if (duration_us == 0) {
        output.status = "zero_duration";
        output.reason = "timing observation has zero duration";
        return output;
    }
    const auto lane_key = cpu_lane_key(pid, tid);
    const auto & cpu = cpu_index();
    const auto nodes = find_nodes(cpu.nodes, lane_key);
    if (nodes.empty()) {
        output.reason = "timing observation has no executable nodes on the same pid/tid lane";
        return output;
    }
    output.start_anchor_node_id = cpu_boundary_at_or_before(pid, tid, output.interval_start_us);
    output.completion_anchor_node_id = cpu_boundary_at_or_after(pid, tid, output.interval_end_us);

    const auto first = std::lower_bound(nodes.begin(), nodes.end(), start_us, [&](size_t node_id, uint64_t timestamp) {
        return graph_.event_for_node(node_id).ts < timestamp;
    });
    const auto last = std::lower_bound(first, nodes.end(), output.interval_end_us, [&](size_t node_id, uint64_t timestamp) {
        return graph_.event_for_node(node_id).ts < timestamp;
    });
    const auto first_index = static_cast<size_t>(first - nodes.begin());
    // Earlier nodes cannot be owned; a prefix maximum still detects any long enclosing leaf.
    bool partial_node_overlap = first_index > 0 && output.interval_end_us > start_us && cpu.prefix_end_us.at(lane_key)[first_index - 1] > start_us;
    for (size_t node_id : std::span<const size_t>{ first, last }) {
        const auto & event = graph_.event_for_node(node_id);
        const auto event_end = index_detail::saturated_add(event.ts, event.dur);
        const auto overlap_start = std::max(output.interval_start_us, event.ts);
        const auto overlap_end = std::min(output.interval_end_us, event_end);
        if (overlap_end <= overlap_start) continue;
        if (event.ts < output.interval_start_us || event_end > output.interval_end_us) {
            partial_node_overlap = true;
            continue;
        }
        output.owned_node_ids.push_back(node_id);
        output.owned_node_duration_us =
            core::checked_add_u64(output.owned_node_duration_us, event.dur, "HiCache timing-owned node duration exceeds uint64 range");
    }

    if (!output.owned_node_ids.empty()) {
        const auto & first = graph_.event_for_node(output.owned_node_ids.front());
        auto frontier_end = index_detail::saturated_add(first.ts, first.dur);
        for (size_t index = 1; index < output.owned_node_ids.size(); ++index) {
            const auto & overlapping = graph_.event_for_node(output.owned_node_ids[index]);
            const auto overlapping_end = index_detail::saturated_add(overlapping.ts, overlapping.dur);
            const auto overlap_end = std::min(frontier_end, overlapping_end);
            if (overlap_end > overlapping.ts) {
                output.has_node_overlap = true;
                break;
            }
            frontier_end = std::max(frontier_end, overlapping_end);
        }
    }

    // Include the gap crossing either boundary; only its successor may start after the interval.
    const auto gap_limit = std::min(nodes.size(), static_cast<size_t>(last - nodes.begin()) + 1);
    for (size_t index = std::max(size_t{ 1 }, first_index); index < gap_limit; ++index) {
        const auto previous_node_id = nodes[index - 1];
        const auto successor_node_id = nodes[index];
        const auto & previous = graph_.event_for_node(previous_node_id);
        const auto & successor = graph_.event_for_node(successor_node_id);
        const auto gap_start = index_detail::saturated_add(previous.ts, previous.dur);
        const auto gap_end = successor.ts;
        if (gap_end <= gap_start) continue;
        const auto owned_start = std::max(output.interval_start_us, gap_start);
        const auto owned_end = std::min(output.interval_end_us, gap_end);
        if (owned_end <= owned_start) continue;
        output.owned_gap_slices.push_back(CpuGapSlice{
            .owner_node_id = previous_node_id,
            .successor_node_id = successor_node_id,
            .gap_start_us = gap_start,
            .gap_end_us = gap_end,
            .owned_start_us = owned_start,
            .owned_end_us = owned_end,
        });
        output.owned_gap_duration_us =
            core::checked_add_u64(output.owned_gap_duration_us, owned_end - owned_start, "HiCache timing-owned CPU gap exceeds uint64 range");
    }

    const auto covered =
        core::checked_add_u64(output.owned_node_duration_us, output.owned_gap_duration_us, "HiCache timing-owned interval exceeds uint64 range");
    constexpr uint64_t timestamp_rounding_tolerance_us = 4;
    if (covered <= output.observed_duration_us) output.uncovered_duration_us = output.observed_duration_us - covered;
    if (partial_node_overlap) output.reason = "timing interval cuts through a retained executable leaf";
    else if (covered > output.observed_duration_us && covered - output.observed_duration_us > timestamp_rounding_tolerance_us)
        output.reason = "same-lane executable intervals overlap inside the timing observation";
    else if (output.uncovered_duration_us > 0) output.reason = "part of the timing interval is outside retained same-lane nodes and CPU gaps";
    else if (!output.start_anchor_node_id || !output.completion_anchor_node_id) output.reason = "timing interval lacks a complete same-lane boundary pair";
    else {
        output.status = "ready";
        output.reason = covered > output.observed_duration_us ? "same pid/tid call containment is complete within four microseconds timestamp rounding"
                                                              : "same pid/tid call containment exactly partitions timing into retained leaves and CPU gaps";
    }
    return output;
}

TimingIntervalOwnership SourceDagIndex::call_interval_ownership(std::string_view pid, std::string_view tid, uint64_t start_us, uint64_t duration_us) const {
    auto ownership = timing_interval_ownership(pid, tid, start_us, duration_us);
    if (!duration_us) return ownership;

    const std::set<size_t> timed(ownership.owned_node_ids.begin(), ownership.owned_node_ids.end());
    ownership.owned_node_ids.clear();
    const auto lane = cpu_nodes_on_lane(pid, tid);
    const auto first = std::ranges::lower_bound(lane, start_us, {}, [&](size_t id) { return graph_.event_for_node(id).ts; });
    for (auto at = first; at != lane.end(); ++at) {
        const auto & event = graph_.event_for_node(*at);
        if (event.ts >= ownership.interval_end_us) break;

        const bool submits = !event.dur && std::ranges::any_of(outgoing_edge_ids(*at), [&](size_t id) {
            const auto & edge = graph_.edge(id);
            return edge.active && edge.kind == core::DagEdgeKind::Correlation && graph_.node(edge.dst).active;
        });
        if (submits || timed.contains(*at)) ownership.owned_node_ids.push_back(*at);
    }
    return ownership;
}

} // namespace markov::trace_graph::source_dag
