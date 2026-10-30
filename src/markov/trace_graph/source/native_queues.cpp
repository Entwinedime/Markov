#include "markov/trace_graph/source/native_queues.hpp"

#include <algorithm>
#include <set>

namespace markov::trace_graph::source {

NativeQueues::NativeQueues(const Observations & observed, const SourceGraph & graph)
    : observed_(observed),
      previous_worker_end_(observed.native_queue_calls.size()),
      runtime_calls_(observed.resources.size()),
      submissions_(observed.resources.size()),
      controls_(observed.resources.size()),
      exclusive_service_(observed.spans.size(), 0) {
    std::vector<std::vector<Index>> workers(observed.resources.size());
    for (Index id = 0; id < observed.native_queue_calls.size(); ++id) workers[observed.native_queue_calls[id].execution.resource].push_back(id);
    for (auto & lane : workers) {
        std::ranges::sort(lane, {}, [&](Index id) { return observed.native_queue_calls[id].execution.begin_ns; });
        for (Index id = 1; id < lane.size(); ++id) {
            const auto & before = observed.native_queue_calls[lane[id - 1]].execution;
            const auto & after = observed.native_queue_calls[lane[id]].execution;
            if (before.end_ns <= after.begin_ns && before.begin_ns < after.begin_ns) previous_worker_end_[lane[id]] = before.end_ns;
        }
    }
    for (Index id = 0; id < observed.native_runtime_calls.size(); ++id) runtime_calls_[observed.native_runtime_calls[id].region.resource].push_back(id);
    for (auto & lane : runtime_calls_) std::ranges::sort(lane, {}, [&](Index id) { return observed.native_runtime_calls[id].region.begin_ns; });
    for (Index id = 0; id < observed.dependencies.size(); ++id) {
        const auto & edge = observed.dependencies[id];
        if (edge.kind != DependencyKind::Synchronization && observed.resources[observed.points[edge.after].resource].kind == ResourceKind::DeviceStream)
            submissions_[observed.points[edge.before].resource].push_back(id);
    }
    for (auto & lane : submissions_) std::ranges::sort(lane, {}, [&](Index id) { return observed.points[observed.dependencies[id].before].time_ns; });
    for (Index id = 0; id < observed.device_controls.size(); ++id) controls_[observed.device_controls[id].submitted.resource].push_back(id);
    for (auto & lane : controls_) std::ranges::sort(lane, {}, [&](Index id) { return observed.device_controls[id].submitted.begin_ns; });
    for (graph::NodeId id = 0; id < graph.graph.nodes().size(); ++id) {
        const auto & node = graph.graph.nodes()[id];
        if (node.kind == graph::NodeKind::Service) exclusive_service_[graph.node_origins[id].span] += node.duration_ns;
    }
    for (Index id = 0; id < observed.spans.size(); ++id) {
        const auto & span = observed.spans[id];
        if (observed.resources[span.resource].kind != ResourceKind::DeviceStream || span.begin_ns == span.end_ns) continue;
        const auto [at, inserted] = device_spans_.try_emplace({ span.resource, span.begin_ns }, id);
        if (!inserted) at->second.reset();
    }
}

NativeQueuePorts NativeQueues::ports(Index id, const planning::NativeCall & definition) const {
    const auto & observed = observed_;
    const auto & call = observed.native_queue_calls.at(id);
    const auto & execution = call.execution;
    NativeQueuePorts result;
    result.launches.resize(definition.launches.size());
    if (previous_worker_end_[id]) {
        const auto ready = std::max(call.submission.begin_ns, *previous_worker_end_[id]);
        result.dispatch = ObservedRegion{ execution.resource, ready, execution.begin_ns, execution.record };
        result.excluded_queue_wait_ns = ready - call.submission.begin_ns;
    }
    if (definition.device_control != planning::DeviceControl::None && result.launches.size() == 1) {
        const auto & lane = controls_[execution.resource];
        const ObservedDeviceControl * command = nullptr;
        bool ambiguous = false;
        for (auto at = std::ranges::lower_bound(lane, execution.begin_ns, {}, [&](Index index) { return observed.device_controls[index].submitted.begin_ns; });
             at != lane.end() && observed.device_controls[*at].submitted.begin_ns < execution.end_ns;
             ++at) {
            const auto & control = observed.device_controls[*at];
            if (control.kind != definition.device_control || control.submitted.end_ns > execution.end_ns
                || control.submitted.record.file != execution.record.file)
                continue;
            if (command) ambiguous = true;
            command = &control;
        }
        const bool ready = command
                           && (command->kind == planning::DeviceControl::Record
                               || (command->producer_completed_ns && *command->producer_completed_ns <= command->command.begin_ns));
        if (ready && !ambiguous) result.launches.front().device = command->command;
    }
    if (definition.kind == planning::CallKind::HostUpload) {
        const auto & lane = runtime_calls_[execution.resource];
        std::optional<ObservedRegion> copy;
        bool ambiguous = false;
        for (auto at =
                 std::ranges::lower_bound(lane, execution.begin_ns, {}, [&](Index index) { return observed.native_runtime_calls[index].region.begin_ns; });
             at != lane.end() && observed.native_runtime_calls[*at].region.begin_ns < execution.end_ns;
             ++at) {
            const auto & runtime = observed.native_runtime_calls[*at];
            if (runtime.region.record.file != execution.record.file || runtime.region.end_ns > execution.end_ns
                || runtime.region.begin_ns == runtime.region.end_ns || observed.names[runtime.implementation] != definition.host_transfer->kind.implementation)
                continue;
            if (copy) ambiguous = true;
            copy = runtime.region;
        }
        if (copy && !ambiguous) {
            result.worker_before = ObservedRegion{ execution.resource, execution.begin_ns, copy->begin_ns, execution.record };
            result.worker_after = ObservedRegion{ execution.resource, copy->end_ns, execution.end_ns, execution.record };
        }
        return result;
    }
    if (definition.launches.empty()) {
        result.worker_after = execution;
        return result;
    }

    const auto & lane = submissions_[execution.resource];
    std::map<std::pair<Time, Index>, std::set<Index>> launches;
    std::set<std::pair<Time, Index>> unresolved;
    for (auto at =
             std::ranges::lower_bound(lane, execution.begin_ns, {}, [&](Index index) { return observed.points[observed.dependencies[index].before].time_ns; });
         at != lane.end() && observed.points[observed.dependencies[*at].before].time_ns <= execution.end_ns;
         ++at) {
        const auto & edge = observed.dependencies[*at];
        const auto & target = observed.points[edge.after];
        const auto submitted = observed.points[edge.before].time_ns;
        const auto key = std::pair(submitted, target.resource);
        auto & spans = launches[key];
        const auto span = device_spans_.find({ target.resource, target.time_ns });
        if (span == device_spans_.end() || !span->second || submitted == execution.end_ns) unresolved.insert(key);
        else spans.insert(*span->second);
    }
    if (launches.size() != definition.launches.size()) return result;

    std::vector<Time> api_ports;
    const auto & runtime_lane = runtime_calls_[execution.resource];
    for (auto at =
             std::ranges::lower_bound(runtime_lane, execution.begin_ns, {}, [&](Index index) { return observed.native_runtime_calls[index].region.begin_ns; });
         at != runtime_lane.end() && observed.native_runtime_calls[*at].region.begin_ns < execution.end_ns;
         ++at) {
        const auto & runtime = observed.native_runtime_calls[*at];
        if (runtime.region.record.file != execution.record.file || runtime.region.end_ns > execution.end_ns) continue;
        if (runtime.device_submission) api_ports.push_back(runtime.region.begin_ns);
        result.code_preparation = result.code_preparation || runtime.code_preparation;
    }
    if (definition.worker_first_use && api_ports.size() != launches.size()) return result;
    const bool ordinary = !definition.worker_first_use || !result.code_preparation;
    auto cursor = execution.begin_ns;
    Index launch = 0;
    for (const auto & [port, spans] : launches) {
        auto & output = result.launches[launch];
        const auto submitted = api_ports.size() == launches.size() ? api_ports[launch] : port.first;
        if (ordinary) output.worker_before = ObservedRegion{ execution.resource, cursor, submitted, execution.record };
        if (definition.device_control == planning::DeviceControl::None && !unresolved.contains(port) && spans.size() == 1) {
            const auto index = *spans.begin();
            const auto & span = observed.spans[index];
            if (exclusive_service_[index] == span.end_ns - span.begin_ns)
                output.device = ObservedRegion{ span.resource, span.begin_ns, span.end_ns, span.record };
        }
        cursor = submitted;
        ++launch;
    }
    if (ordinary) result.worker_after = ObservedRegion{ execution.resource, cursor, execution.end_ns, execution.record };
    return result;
}

} // namespace markov::trace_graph::source
