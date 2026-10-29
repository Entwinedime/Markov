#include "markov/trace_graph/source/context.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
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
    frame_results_.resize(observations_.compute_frames.size());
    for (Index id = 0; id < observations_.batch_results.size(); ++id) {
        auto & result = frame_results_.at(observations_.batch_results[id].frame);
        if (result) throw std::runtime_error("Multiple batch results refer to one compute frame");
        result = id;
    }
    device_services_ = bind_device_services(observations_, graph_);
    index_cpu_intervals();
    index_native_sequences();
    index_native_work();
    index_compute_queues();
    index_frame_calls();

    // The context is read-only now; keep semantic tables, not parser interning.
    observations_.release_intake_indexes();
}

void SourceContext::index_native_sequences() {
    std::vector<std::vector<Index>> lanes(observations_.resources.size());
    for (Index id = 0; id < observations_.native_queue_calls.size(); ++id) lanes[observations_.native_queue_calls[id].submission.resource].push_back(id);
    const auto submitted = [&](Index id) { return observations_.native_queue_calls[id].submission.begin_ns; };
    for (auto & lane : lanes) std::ranges::sort(lane, {}, submitted);

    for (const auto & frame : observations_.compute_frames) {
        auto & bindings = preparation_native_calls_.emplace_back(frame.preparations.size());
        if (frame.sequence_tokens.size() != 1 || !frame.query_tokens) continue;
        const auto query = std::accumulate(frame.query_tokens->begin(), frame.query_tokens->end(), 0.0);
        const auto context = frame.sequence_tokens.front();
        const planning::FrameInput input{ query, query, context, 1, frame.page_tokens.value_or(0) };
        for (Index id = 0; id < frame.preparations.size(); ++id) {
            const auto & preparation = frame.preparations[id];
            if (!preparation.definition) continue;
            auto sequence = preparation.definition->describe(frame.phase, input);
            if (sequence.issue || sequence.calls.empty()
                || std::ranges::any_of(sequence.calls, [](const planning::NativeCall & call) { return call.queue_name.empty(); }))
                continue;

            auto & binding = bindings[id];
            const auto & region = preparation.region;
            const auto & lane = lanes[region.resource];
            auto at = std::ranges::lower_bound(lane, region.begin_ns, {}, submitted);
            for (; at != lane.end() && submitted(*at) < region.end_ns; ++at) binding.calls.push_back(*at);
            if (binding.calls.size() != sequence.calls.size()) {
                binding.issue = "native_queue_call_count_mismatch";
                continue;
            }
            binding.issue.reset();
            for (Index call = 0; call < binding.calls.size(); ++call) {
                const auto & native = observations_.native_queue_calls[binding.calls[call]];
                if (observations_.names[native.name] != sequence.calls[call].queue_name || native.submission.end_ns > region.end_ns) {
                    binding.issue = "native_queue_call_identity_mismatch";
                    break;
                }
                if (call) {
                    const auto & previous = observations_.native_queue_calls[binding.calls[call - 1]];
                    if (previous.submission.end_ns > native.submission.begin_ns || previous.execution.resource != native.execution.resource
                        || previous.execution.end_ns > native.execution.begin_ns) {
                        binding.issue = "native_queue_order_unavailable";
                        break;
                    }
                }
            }
            if (!binding.issue) binding.definitions = std::move(sequence.calls);
        }
    }
}

void SourceContext::index_frame_calls() {
    struct Entry {
        FrameCall call;
        ObservedRegion region;
    };
    frame_calls_.resize(frame_operations_.size());
    frame_cpu_order_supported_.resize(frame_operations_.size(), true);
    for (Index frame = 0; frame < frame_operations_.size(); ++frame) {
        std::vector<Entry> entries;
        for (const auto id : frame_operations_[frame]) {
            const auto & call = observations_.work_observations[id];
            entries.push_back({
                { FrameCallKind::Compute, id },
                { call.resource, call.begin_ns, call.end_ns, call.record }
            });
        }
        const auto & preparations = observations_.compute_frames[frame].preparations;
        for (Index id = 0; id < preparations.size(); ++id)
            entries.push_back({
                { FrameCallKind::Preparation, id },
                preparations[id].region
            });
        for (const auto id : frame_collectives_[frame]) {
            const auto & call = observations_.collectives[id];
            entries.push_back({
                { FrameCallKind::Collective, id },
                { call.resource, call.begin_ns, call.end_ns, call.record }
            });
        }
        std::ranges::stable_sort(entries, {}, [](const Entry & entry) { return entry.region.begin_ns; });
        for (Index i = 1; i < entries.size(); ++i) {
            const auto & before = entries[i - 1].region;
            const auto & after = entries[i].region;
            if (before.resource != after.resource || before.end_ns > after.begin_ns) frame_cpu_order_supported_[frame] = false;
        }
        for (Index i = 0; i < entries.size(); ++i) {
            auto call = entries[i].call;
            if (frame_cpu_order_supported_[frame] && i && entries[i - 1].region.end_ns < entries[i].region.begin_ns) {
                call.gap_before = cpu_gaps_.size();
                const auto & after = entries[i].region;

                // The following semantic record anchors this inter-call region.
                cpu_gaps_.push_back({ after.resource, entries[i - 1].region.end_ns, after.begin_ns, after.record });
            }
            frame_calls_[frame].push_back(call);
        }
    }

    // A gap containing a native submission cannot stand in for that missing
    // work: its worker/device lifecycle must be generated explicitly.
    cpu_gap_native_submissions_.resize(cpu_gaps_.size(), false);
    std::vector<std::vector<Index>> lanes(observations_.resources.size());
    for (Index id = 0; id < cpu_gaps_.size(); ++id) lanes[cpu_gaps_[id].resource].push_back(id);
    for (auto & lane : lanes) std::ranges::sort(lane, {}, [&](Index id) { return cpu_gaps_[id].begin_ns; });
    for (const auto & edge : observations_.dependencies) {
        if (edge.kind == DependencyKind::Synchronization) continue;
        const auto & point = observations_.points[edge.before];
        if (point.resource == observations_.points[edge.after].resource) continue;
        const auto & lane = lanes[point.resource];
        const auto at = std::ranges::upper_bound(lane, point.time_ns, {}, [&](Index id) { return cpu_gaps_[id].begin_ns; });
        if (at != lane.begin() && point.time_ns < cpu_gaps_[*std::prev(at)].end_ns) cpu_gap_native_submissions_[*std::prev(at)] = true;
    }
}

void SourceContext::index_cpu_intervals() {
    std::vector<ObservedRegion> regions;
    regions.reserve(observations_.work_observations.size());
    for (const auto & work : observations_.work_observations) regions.push_back({ work.resource, work.begin_ns, work.end_ns, work.record });
    for (const auto & frame : observations_.compute_frames)
        for (const auto & call : frame.preparations) regions.push_back(call.region);

    auto intervals = cpu_intervals(observations_, graph_, regions);
    auto at = observations_.work_observations.size();
    auto resources = call_resources(observations_, regions);
    for (const auto & frame : observations_.compute_frames) {
        preparation_intervals_.emplace_back(intervals.begin() + at, intervals.begin() + at + frame.preparations.size());
        preparation_bindings_.emplace_back(std::make_move_iterator(resources.begin() + at),
                                           std::make_move_iterator(resources.begin() + at + frame.preparations.size()));
        at += frame.preparations.size();
    }
    compute_resources_.reserve(observations_.work_observations.size());
    for (Index id = 0; id < observations_.work_observations.size(); ++id) compute_resources_.push_back(resources[id].unique_resources());
    intervals.resize(observations_.work_observations.size());
    cpu_work_intervals_ = std::move(intervals);
}

std::span<const Index> SourceContext::request_frames(std::string_view request) const {
    const auto found = request_frames_.find(request);
    return found == request_frames_.end() ? std::span<const Index>{} : found->second;
}

const BatchResult * SourceContext::frame_result(Index frame) const {
    const auto result = frame_results_[frame];
    return result ? &observations_.batch_results[*result] : nullptr;
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
