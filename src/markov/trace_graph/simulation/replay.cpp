#include "markov/trace_graph/simulation/replay.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>

namespace markov::trace_graph::simulation {

ReplayResult replay(const graph::Graph & graph, std::span<const graph::Time> initial_availability_ns) {
    const auto nodes = graph.nodes();
    ReplayResult result{
        { initial_availability_ns.begin(), initial_availability_ns.end() },
        std::vector<graph::Time>(nodes.size()),
        0
    };
    std::vector<std::size_t> incoming(graph.incoming_counts().begin(), graph.incoming_counts().end());
    std::deque<graph::NodeId> ready;
    for (graph::NodeId id = 0; id < nodes.size(); ++id) {
        if (incoming[id] == 0) ready.push_back(id);
    }

    while (!ready.empty()) {
        const auto id = ready.front();
        ready.pop_front();
        const auto end = result.starts_ns[id] + nodes[id].duration_ns;
        result.completions_ns[id] = end;
        ++result.executed;
        for (const auto & edge : graph.successors(id)) {
            result.starts_ns[edge.to] = std::max(result.starts_ns[edge.to], end);
            if (--incoming[edge.to] == 0) ready.push_back(edge.to);
        }
    }
    if (result.executed != nodes.size()) throw std::runtime_error("Observed dependencies contain a cycle");

    return result;
}

} // namespace markov::trace_graph::simulation
