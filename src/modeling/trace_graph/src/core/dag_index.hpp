#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::core {

/** Immutable index snapshots. Readers retain ownership across graph edits. */
struct DagAdjacency {
    std::vector<size_t> incoming_offsets, incoming_edge_ids;
    std::vector<size_t> outgoing_offsets, outgoing_edge_ids;

    std::span<const size_t> incoming(size_t node) const {
        return std::span<const size_t>(incoming_edge_ids).subspan(incoming_offsets[node], incoming_offsets[node + 1] - incoming_offsets[node]);
    }

    std::span<const size_t> outgoing(size_t node) const {
        return std::span<const size_t>(outgoing_edge_ids).subspan(outgoing_offsets[node], outgoing_offsets[node + 1] - outgoing_offsets[node]);
    }
};

struct CpuIntervalIndex {
    using NodeMap = std::unordered_map<std::string, std::vector<size_t>, TraceArgHash, std::equal_to<>>;
    static std::string lane_key(std::string_view pid, std::string_view tid) { return std::string(pid) + "\x1f" + std::string(tid); }
    NodeMap nodes;
    std::unordered_map<std::string, std::vector<uint64_t>, TraceArgHash, std::equal_to<>> prefix_end_us;
};

struct DagTopologicalOrder {
    // Inactive identities also have positions; only active edges constrain order.
    std::vector<size_t> nodes, positions;
};

} // namespace markov::trace_graph::core
