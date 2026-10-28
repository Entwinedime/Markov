#include "markov/trace_graph/planning/native_calls.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace markov::trace_graph::planning {

void NativePlan::depend(std::optional<graph::NodeId> before, graph::NodeId after) {
    if (before) edges_.push_back({ *before, after, graph::EdgeKind::DataDependency });
}

graph::NodeId NativePlan::append(const Work & work, const char * scope, std::size_t resource, std::optional<graph::NodeId> & tail, bool delay) {
    const auto id = work_.size();
    work_.push_back(work);
    work_.back().kind.timing_scope = scope;
    steps_.push_back({ resource, delay });
    depend(tail, id);
    tail = id;
    return id;
}

void NativePlan::cpu(const Work & work, const char * service, const char * residual, std::size_t resource, std::optional<graph::NodeId> & tail) {
    append(work, service, resource, tail);
    append(work, residual, resource, tail);
}

NativePlan::NativePlan(std::span<const NativeCall> calls, CallResources resources) {
    for (const auto & call : calls) {
        cpu(call.work, "cpu_service", "residual_interval", resources.caller, caller_);
        if (call.kind == CallKind::Cpu) continue;

        auto returned = call.work;
        returned.kind.operation += ".return";
        if (call.kind == CallKind::Readback) {
            const auto & synchronization = call.synchronization.value();
            const auto ready = append(synchronization, synchronization.kind.timing_scope.c_str(), resources.caller, caller_);
            depend(device_, ready);
            depend(worker_, ready);
            append(returned, "post_sync_readback_interval", resources.caller, caller_);
            continue;
        }

        auto dispatch = caller_;
        const auto ready = append(call.work, "dispatch_interval", resources.worker, dispatch, true);

        // Dispatch samples start after both submission and the previous task.
        depend(worker_, ready);
        worker_ = ready;
        for (const auto & launch : call.launches) {
            cpu(launch, "worker_launch_service", "worker_launch_residual_interval", resources.worker, worker_);
            const auto kernel = append(launch, "device_service", resources.device, device_);
            depend(worker_, kernel);
        }
        cpu(call.work, "worker_return_service", "worker_return_residual_interval", resources.worker, worker_);
        cpu(returned, "cpu_service", "residual_interval", resources.caller, caller_);
    }
}

std::size_t NativePlan::delays() const { return std::ranges::count(steps_, true, &Step::delay); }

PlannedMutation NativePlan::mutation(simulation::Component owner, graph::NodeId begin, simulation::Action completion, graph::NodeId cause) const {
    PlannedMutation result{ owner, {}, {} };
    std::vector<bool> has_input(work_.size(), false);
    for (std::size_t id = 0; id < work_.size(); ++id)
        result.operations.push_back({ steps_[id].resource, std::cref(work_[id]), std::nullopt, steps_[id].delay });
    for (const auto & edge : edges_) {
        result.dependencies.push_back({ begin + edge.from, begin + edge.to, edge.kind });
        has_input[edge.to] = true;
    }
    for (std::size_t id = 0; id < has_input.size(); ++id)
        if (!has_input[id]) result.dependencies.push_back({ cause, begin + id, graph::EdgeKind::DataDependency });

    const auto end = begin + work_.size();
    result.operations.push_back({ 0, Boundary{}, completion });
    for (const auto tail : { caller_, worker_, device_ })
        if (tail) result.dependencies.push_back({ begin + *tail, end, graph::EdgeKind::DataDependency });
    if (work_.empty()) result.dependencies.push_back({ cause, end, graph::EdgeKind::DataDependency });
    return result;
}

} // namespace markov::trace_graph::planning
