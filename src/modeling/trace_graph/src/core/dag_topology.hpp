#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace markov::trace_graph::core::detail {

/**
 * Locate one directed cycle in the unresolved part of a validated CSR graph.
 * Offsets contain one sentinel after the last node; destinations are valid node
 * IDs. Remaining indegrees cover those nodes, with inactive/consumed nodes zero.
 * Traversal preserves node and edge order and never modifies the input. An empty
 * result means no cycle was found (a CPU queue can stall for other reasons).
 */
inline std::vector<size_t> find_unresolved_cycle(std::span<const size_t> offsets, std::span<const size_t> destinations,
                                                 std::span<const size_t> remaining_indegree) {
    struct Frame {
        size_t node;
        size_t next_offset;
    };
    const auto node_count = offsets.empty() ? 0 : offsets.size() - 1;
    std::vector<uint8_t> state(node_count, 0);
    std::vector<size_t> position(node_count, 0);
    std::vector<size_t> path;
    std::vector<Frame> stack;
    for (size_t start = 0; start < node_count; ++start) {
        if (remaining_indegree[start] == 0 || state[start] != 0) continue;
        state[start] = 1;
        position[start] = 0;
        path.push_back(start);
        stack.push_back({ start, offsets[start] });
        while (!stack.empty()) {
            auto & frame = stack.back();
            if (frame.next_offset == offsets[frame.node + 1]) {
                state[frame.node] = 2;
                stack.pop_back();
                path.pop_back();
                continue;
            }
            const auto dst = destinations[frame.next_offset++];
            if (remaining_indegree[dst] == 0) continue;
            if (state[dst] == 1) return { path.begin() + static_cast<std::ptrdiff_t>(position[dst]), path.end() };
            if (state[dst] == 0) {
                state[dst] = 1;
                position[dst] = path.size();
                path.push_back(dst);
                stack.push_back({ dst, offsets[dst] });
            }
        }
    }
    return {};
}

} // namespace markov::trace_graph::core::detail
