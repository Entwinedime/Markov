#include "markov/trace_graph/source/context.hpp"

#include <algorithm>
#include <limits>
#include <map>
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
    frame_collectives_.resize(observations_.compute_frames.size());
    for (Index id = 0; id < observations_.collectives.size(); ++id) frame_collectives_[observations_.collectives[id].frame].push_back(id);
    for (auto & calls : frame_collectives_) std::ranges::sort(calls, {}, [&](Index id) { return observations_.collectives[id].begin_ns; });
    for (Index frame = 0; frame < observations_.compute_frames.size(); ++frame) {
        for (const auto request : observations_.compute_frames[frame].requests) request_frames_[observations_.names[request]].push_back(frame);
    }
    device_services_ = bind_device_services(observations_, graph_);
    index_compute_queues();

    // The context is read-only now; keep semantic tables, not parser interning.
    observations_.release_intake_indexes();
}

std::span<const Index> SourceContext::request_frames(std::string_view request) const {
    const auto found = request_frames_.find(request);
    return found == request_frames_.end() ? std::span<const Index>{} : found->second;
}

void SourceContext::index_compute_queues() {
    frame_queue_order_.resize(frame_operations_.size());
    frame_queue_issues_.resize(frame_operations_.size());
    for (Index frame = 0; frame < frame_operations_.size(); ++frame) {
        std::map<Index, std::vector<Index>> queues;
        auto & issues = frame_queue_issues_[frame];
        for (const auto operation : frame_operations_[frame]) {
            const auto & service = device_services_[operation];
            if (service.resource) queues[*service.resource].push_back(operation);
            else issues.push_back(operation);
        }
        for (auto & [resource, operations] : queues) {
            // Source node IDs are in queue order within one resource. Preserve
            // that relation, rather than ordering target work by estimated time.
            std::ranges::sort(operations, {}, [&](Index id) { return device_services_[id].nodes.front(); });
            for (std::size_t i = 1; i < operations.size(); ++i) {
                const auto before = operations[i - 1], after = operations[i];
                if (device_services_[before].nodes.back() >= device_services_[after].nodes.front()) {
                    issues.push_back(before);
                    issues.push_back(after);
                }
                else frame_queue_order_[frame].push_back({ before, after });
            }
        }
        std::ranges::sort(issues);
        issues.erase(std::unique(issues.begin(), issues.end()), issues.end());
    }
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
