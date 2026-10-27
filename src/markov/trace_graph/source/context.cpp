#include "markov/trace_graph/source/context.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace markov::trace_graph::source {

SourceContext::SourceContext(Observations && observations)
    : observations_(std::move(observations)),
      graph_(build_source_graph(observations_)),
      origin_ns_(std::numeric_limits<Time>::max()) {
    for (const auto & node : graph_.node_origins) origin_ns_ = std::min(origin_ns_, node.begin_ns);

    // The context is read-only now; keep semantic tables, not parser interning.
    observations_.release_intake_indexes();
}

simulation::ReplayResult SourceContext::replay() const {
    std::vector<Time> available(graph_.graph.nodes().size(), 0);
    for (Index i = 0; i < available.size(); ++i) {
        if (graph_.graph.incoming_counts()[i] == 0) available[i] = graph_.node_origins[i].begin_ns - origin_ns_;
    }
    return simulation::replay(graph_.graph, available);
}

} // namespace markov::trace_graph::source
