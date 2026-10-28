#include "markov/trace_graph/planning/native_calls.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace markov::trace_graph::planning {

KernelUse PreparationCache::use(const Work & specialization, std::size_t caller) {
    if (initial_ == KernelCacheInitial::Prepared || !prepared_[caller].insert(specialization).second) return KernelUse::Prepared;

    // shortcut: one launcher ABI per caller; separate ABI loading when another family is modeled.
    return loaded_.insert(caller).second ? KernelUse::FirstLoad : KernelUse::Load;
}

std::size_t PreparationCache::specializations() const {
    std::size_t count = 0;
    for (const auto & [caller, keys] : prepared_) count += keys.size();
    return count;
}

void NativePlan::depend(std::optional<graph::NodeId> before, graph::NodeId after) {
    if (before) edges_.push_back({ *before, after, graph::EdgeKind::DataDependency });
}

graph::NodeId NativePlan::append(const Work & work, const char * scope, std::size_t resource, std::optional<graph::NodeId> & tail, bool delay) {
    const auto id = steps_.size();
    auto owned = work;
    owned.kind.timing_scope = scope;
    steps_.push_back({ std::move(owned), resource, delay });
    depend(tail, id);
    tail = id;
    return id;
}

void NativePlan::source_cpu_gap(std::size_t interval, std::size_t caller) {
    const auto id = steps_.size();
    steps_.push_back({ SourceCpuGap{ interval }, caller, true });
    depend(caller_, id);
    caller_ = id;
}

void NativePlan::cpu(const Work & work, const char * service, const char * residual, std::size_t resource, std::optional<graph::NodeId> & tail) {
    append(work, service, resource, tail);
    append(work, residual, resource, tail);
}

bool NativePlan::extend(std::span<const NativeCall> calls, CallResources resources, PreparationCache & preparations) {
    for (const auto & call : calls) {
        if (call.preparation) {
            if (!preparations.initial()) return false;
            const auto & preparation = *call.preparation;
            const auto use = preparations.use(preparation.specialization, resources.caller);
            if (use != KernelUse::Prepared) {
                append(preparation.compilation, preparation.compilation.kind.timing_scope.c_str(), resources.caller, caller_);
                const auto & load = use == KernelUse::FirstLoad ? preparation.first_load : preparation.load;
                append(load, load.kind.timing_scope.c_str(), resources.caller, caller_);
            }
        }
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
            steps_[kernel].source_operation = call.source_operation;
            depend(worker_, kernel);
        }
        cpu(call.work, "worker_return_service", "worker_return_residual_interval", resources.worker, worker_);
        cpu(returned, "cpu_service", "residual_interval", resources.caller, caller_);
    }
    return true;
}

std::size_t NativePlan::delays() const {
    return std::ranges::count_if(steps_, [](const Step & step) { return step.delay && std::holds_alternative<Work>(step.cost); });
}

PlannedMutation NativePlan::mutation(simulation::Component owner, graph::NodeId begin, simulation::Action completion,
                                     std::optional<graph::NodeId> cause) const {
    PlannedMutation result{ owner, {}, {} };
    std::vector<bool> has_input(steps_.size(), false);
    for (const auto & step : steps_) {
        if (const auto * work = std::get_if<Work>(&step.cost))
            result.operations.push_back({ step.resource, std::cref(*work), std::nullopt, step.delay, step.source_operation });
        else result.operations.push_back({ step.resource, std::get<SourceCpuGap>(step.cost), std::nullopt, true });
    }
    for (const auto & edge : edges_) {
        result.dependencies.push_back({ begin + edge.from, begin + edge.to, edge.kind });
        has_input[edge.to] = true;
    }
    for (std::size_t id = 0; id < has_input.size(); ++id)
        if (cause && !has_input[id]) result.dependencies.push_back({ *cause, begin + id, graph::EdgeKind::DataDependency });

    const auto end = begin + steps_.size();
    result.operations.push_back({ 0, Boundary{}, completion });
    for (const auto tail : { caller_, worker_, device_ })
        if (tail) result.dependencies.push_back({ begin + *tail, end, graph::EdgeKind::DataDependency });
    if (steps_.empty() && cause) result.dependencies.push_back({ *cause, end, graph::EdgeKind::DataDependency });
    return result;
}

} // namespace markov::trace_graph::planning
