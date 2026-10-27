#pragma once

#include "markov/trace_graph/graph/graph.hpp"

#include <span>
#include <vector>

namespace markov::trace_graph::simulation {

struct ReplayResult {
    std::vector<graph::Time> starts_ns;
    std::vector<graph::Time> completions_ns;
    std::size_t executed;
};

// Schedule dependency-ready work. Resource order is represented by graph edges.
// One initial availability is required for each node. Observed root coordinates are supplied
// by source replay only and are not implicit prediction release times.
ReplayResult replay(const graph::Graph & graph, std::span<const graph::Time> initial_availability_ns);

} // namespace markov::trace_graph::simulation
