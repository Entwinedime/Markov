#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace markov::trace_graph::core {

class DagGraph;

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
    size_t node_count = 0, event_count = 0;
};

/** Immutable source queue coordinates, identities and compiled native tasks.
 * Queue binding combines these with current activity and event overrides. */
struct CpuTaskObservations {
    using Identity = std::tuple<int, std::string, std::string>;
    struct Member {
        size_t node, identity;
        uint64_t start_us, end_us;
    };

    // Member ranges index the compiled node list, not the trace-event array.
    struct Task {
        size_t first, last, submission, identity, member_begin, member_end;
        uint64_t submission_end_us, ready_delay_us, submission_overlap_us;
    };

    struct QueueProgram {
        std::vector<size_t> members;
        std::vector<Task> tasks;
        bool complete = false;
        std::optional<size_t> missing_submission;
    };

    size_t node_count = 0, edge_count = 0, event_count = 0;
    std::map<Identity, size_t> identities;
    std::map<size_t, std::vector<Member>> lanes;
    std::vector<std::pair<size_t, size_t>> submissions;
    std::vector<size_t> task_edges;
    // Physical queues and native task grouping are source facts. Activity and
    // FIFO arrival order are bound separately for each target.
    std::map<size_t, QueueProgram> queue_programs;
};

[[nodiscard]] CpuTaskObservations::QueueProgram compile_cpu_queue_program(const DagGraph & graph, std::span<const CpuTaskObservations::Member> members,
                                                                          std::span<const size_t> submissions);

struct DagTopologicalOrder {
    // Inactive identities also have positions; only active edges constrain order.
    std::vector<size_t> nodes, positions;
};

} // namespace markov::trace_graph::core
