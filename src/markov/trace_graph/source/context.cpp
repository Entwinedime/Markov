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

    frame_operations_.resize(observations_.compute_frames.size());
    for (Index id = 0; id < observations_.work_observations.size(); ++id) frame_operations_[observations_.work_observations[id].frame].push_back(id);
    for (auto & operations : frame_operations_) {
        std::ranges::stable_sort(operations, {}, [&](Index id) { return observations_.work_observations[id].begin_ns; });
    }
    for (Index frame = 0; frame < observations_.compute_frames.size(); ++frame) {
        for (const auto request : observations_.compute_frames[frame].requests) request_frames_[observations_.names[request]].push_back(frame);
    }
    device_services_ = bind_device_services(observations_, graph_);

    // The context is read-only now; keep semantic tables, not parser interning.
    observations_.release_intake_indexes();
}

std::span<const Index> SourceContext::request_frames(std::string_view request) const {
    const auto found = request_frames_.find(request);
    return found == request_frames_.end() ? std::span<const Index>{} : found->second;
}

simulation::ExecutionResult SourceContext::replay() const {
    std::vector<Time> available(graph_.graph.nodes().size(), 0);
    for (Index i = 0; i < available.size(); ++i) {
        if (graph_.graph.incoming_counts()[i] == 0) available[i] = graph_.node_origins[i].begin_ns - origin_ns_;
    }
    simulation::Execution execution(graph_.graph, available);
    execution.run();
    return execution.take_result();
}

} // namespace markov::trace_graph::source
