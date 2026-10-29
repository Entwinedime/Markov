#include "markov/trace_graph/planning/native_calls.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::planning {

void CallerSchedule::acquire(ProgramRuntime & runtime, std::size_t caller, simulation::Component owner, simulation::Action granted,
                             std::optional<graph::NodeId> cause) {
    auto & lane = lanes_[caller];
    lane.pending.push_back({ owner, granted, cause });
    dispatch(runtime, caller, lane);
}

void CallerSchedule::release(ProgramRuntime & runtime, std::size_t caller, simulation::Component owner, graph::NodeId completion) {
    auto & lane = lanes_.at(caller);
    if (lane.owner != owner) throw std::logic_error("Caller released by another program");
    lane.owner.reset();
    lane.completion = completion;
    dispatch(runtime, caller, lane);
}

void CallerSchedule::handoff(ProgramRuntime & runtime, std::size_t caller, simulation::Component previous, simulation::Component next,
                             simulation::Action granted, graph::NodeId cause) {
    lanes_.at(caller).pending.push_front({ next, granted, cause });
    release(runtime, caller, previous, cause);
}

void CallerSchedule::dispatch(ProgramRuntime & runtime, std::size_t caller, Lane & lane) {
    if (lane.owner || lane.pending.empty()) return;
    const auto request = lane.pending.front();
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ request.owner, { { caller, Boundary{}, request.granted } }, {} };
    if (request.cause) mutation.dependencies.push_back({ *request.cause, begin, graph::EdgeKind::DataDependency });
    if (lane.completion) mutation.dependencies.push_back({ *lane.completion, begin, graph::EdgeKind::ResourceOrder });
    if (!runtime.submit(std::move(mutation))) return;
    lane.owner = request.owner;
    lane.pending.pop_front();
}

Work NativeCall::worker_work(const Work & operation) const {
    auto result = operation;
    if (worker_features) result.features = *worker_features;
    return result;
}

KernelUse PreparationCache::use(const KernelPreparation & preparation, std::size_t caller) {
    if (initial_ == KernelCacheInitial::Prepared || !prepared_[caller].insert(preparation.specialization).second) return KernelUse::Prepared;

    return loaded_[caller].insert(preparation.first_load).second ? KernelUse::FirstLoad : KernelUse::Load;
}

bool PreparationCache::first_use(const Work & preparation, std::size_t resource) {
    return initial_ != KernelCacheInitial::Prepared && prepared_[resource].insert(preparation).second;
}

std::size_t PreparationCache::specializations() const {
    std::size_t count = 0;
    for (const auto & [caller, keys] : prepared_) count += keys.size();
    return count;
}

std::size_t PreparationCache::first_loads() const {
    std::size_t count = 0;
    for (const auto & [caller, launchers] : loaded_) count += launchers.size();
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

void NativePlan::cpu(const Work & work, const char * service, const char * residual, std::size_t resource, std::optional<graph::NodeId> & tail,
                     std::optional<std::size_t> source) {
    steps_[append(work, service, resource, tail)].source_native_work = source;
    steps_[append(work, residual, resource, tail)].source_native_work = source;
}

bool NativePlan::extend(const NativeSequence & sequence, CallResources resources, PreparationCache & preparations) {
    const auto incoming = producer_device();
    for (const auto & call : sequence.calls) {

        // shortcut: caller prerequisites share one submission port; split the prelude if intermediate results gain external consumers.
        for (std::size_t id = 0; id < call.caller_preparations.size(); ++id) {
            const auto & preparation = call.caller_preparations[id];
            const auto node = append(preparation.work, preparation.work.kind.timing_scope.c_str(), resources.caller, caller_);
            if (!call.sources.caller_preparations.empty()) steps_[node].source_native_work = call.sources.caller_preparations[id];
            if (call.kind == CallKind::Readback) {
                depend(device_ ? std::optional(device_->completion) : std::nullopt, node);
                depend(worker_, node);
                preceding_waits_.emplace_back(node, resources.worker);
                preceding_waits_.emplace_back(node, resources.device);
            }
        }
        if (call.preparation) {
            if (!preparations.initial()) return false;
            const auto & preparation = *call.preparation;
            const auto initialized = preparations.runtime_initialized(resources.caller);
            const auto use = preparations.use(preparation, resources.caller);
            if (use != KernelUse::Prepared) {
                append(preparation.compilation, preparation.compilation.kind.timing_scope.c_str(), resources.caller, caller_);
                auto load = use == KernelUse::FirstLoad ? preparation.first_load : preparation.load;
                if (use == KernelUse::FirstLoad) load.features["runtime_initialized"] = initialized ? 1 : 0;
                append(load, load.kind.timing_scope.c_str(), resources.caller, caller_);
            }
        }
        if (call.kind == CallKind::Readback) {
            const auto & synchronization = call.synchronization.value();
            const auto ready = append(synchronization, synchronization.kind.timing_scope.c_str(), resources.caller, caller_);
            depend(device_ ? std::optional(device_->completion) : std::nullopt, ready);
            depend(worker_, ready);
            preceding_waits_.emplace_back(ready, resources.worker);
            preceding_waits_.emplace_back(ready, resources.device);
        }
        const auto & caller_work = call.caller_work ? *call.caller_work : call.work;
        cpu(caller_work, "cpu_service", "residual_interval", resources.caller, caller_, call.sources.caller_before);
        if (call.kind == CallKind::Cpu || call.kind == CallKind::Readback) continue;

        auto returned = caller_work;
        returned.kind.operation += ".return";
        auto dispatch = caller_;
        const auto worker_work = call.worker_work(call.work);
        const auto ready = append(worker_work, "dispatch_interval", resources.worker, dispatch, true);
        steps_[ready].source_native_work = call.sources.dispatch;

        // Dispatch samples start after both submission and the previous task.
        depend(worker_, ready);
        worker_ = ready;
        if (call.worker_first_use) {
            if (!preparations.initial()) return false;

            // shortcut: closed-callback startup precedes its body; split it when intermediate results gain external consumers.
            if (preparations.first_use(*call.worker_first_use, resources.worker))
                append(*call.worker_first_use, call.worker_first_use->kind.timing_scope.c_str(), resources.worker, worker_);
        }
        const auto device = call.incoming_stream ? incoming.value() : resources.device;
        if (call.kind == CallKind::HostUpload) {
            cpu(worker_work, "worker_launch_service", "worker_launch_residual_interval", resources.worker, worker_, call.sources.worker_before);
            const auto & transfer = call.host_transfer.value();
            const auto copied = append(transfer, transfer.kind.timing_scope.c_str(), resources.worker, worker_);
            depend(device_ ? std::optional(device_->completion) : std::nullopt, copied);
            preceding_waits_.emplace_back(copied, device);

            // Synchronous host copies produce device data without issuing a
            // device task. Keep its location separate from the executing CPU.
            device_ = DeviceProducer{ copied, device };
        }
        auto device_tail = device_ ? std::optional(device_->completion) : std::nullopt;
        for (std::size_t id = 0; id < call.launches.size(); ++id) {
            const auto & launch = call.launches[id];
            const auto sources = call.sources.launches.empty() ? NativeLaunchSources{} : call.sources.launches[id];
            cpu(call.worker_work(launch), "worker_launch_service", "worker_launch_residual_interval", resources.worker, worker_, sources.worker_before);
            const auto kernel = append(launch, "device_service", device, device_tail);
            device_ = DeviceProducer{ kernel, device };
            steps_[kernel].source_operation = call.source_operation;
            steps_[kernel].source_native_work = sources.device;
            depend(worker_, kernel);
        }
        cpu(worker_work, "worker_return_service", "worker_return_residual_interval", resources.worker, worker_, call.sources.worker_after);
        if (call.kind == CallKind::HostUpload) {
            const auto & synchronization = call.synchronization.value();
            const auto synchronized = append(synchronization, synchronization.kind.timing_scope.c_str(), resources.caller, caller_);
            depend(worker_, synchronized);
        }
        cpu(returned, "cpu_service", "residual_interval", resources.caller, caller_, call.sources.caller_after);
    }
    if (sequence.result_ready) {
        if (*sequence.result_ready == NativeProducer::Caller) result_ = caller_;
        else result_ = device_ ? std::optional(device_->completion) : std::nullopt;
        if (!result_) throw std::logic_error("Native sequence declares a result without its producer");
    }
    return true;
}

std::size_t NativePlan::delays() const {
    return std::ranges::count_if(steps_, [](const Step & step) { return step.delay && std::holds_alternative<Work>(step.cost); });
}

NativeFrontier NativePlan::frontier(graph::NodeId begin) const {
    NativeFrontier result;
    for (std::size_t id = 0; id < steps_.size(); ++id) result[steps_[id].resource] = begin + id;
    return result;
}

PlannedMutation NativePlan::mutation(simulation::Component owner, graph::NodeId begin, NativeEvents events, std::optional<graph::NodeId> cause,
                                     const NativeFrontier & preceding) const {
    PlannedMutation result{ owner, {}, {} };
    std::vector<bool> has_input(steps_.size(), false);
    std::set<std::size_t> linked;
    for (const auto & step : steps_) {
        // Readiness alone does not preserve FIFO while earlier work is blocked.
        if (linked.insert(step.resource).second) {
            const auto previous = preceding.find(step.resource);
            if (previous != preceding.end())
                result.dependencies.push_back({ previous->second, begin + result.operations.size(), graph::EdgeKind::ResourceOrder });
        }
        if (const auto * work = std::get_if<Work>(&step.cost))
            result.operations.push_back({ step.resource, std::cref(*work), std::nullopt, step.delay, step.source_operation, step.source_native_work });
        else result.operations.push_back({ step.resource, std::get<SourceCpuGap>(step.cost), std::nullopt, true });
    }
    for (const auto & edge : edges_) {
        result.dependencies.push_back({ begin + edge.from, begin + edge.to, edge.kind });
        has_input[edge.to] = true;
    }
    // A synchronous read can consume the preceding queue before this segment
    // has submitted any work of its own to that queue.
    for (const auto & [node, resource] : preceding_waits_) {
        const auto previous = preceding.find(resource);
        if (previous != preceding.end()) result.dependencies.push_back({ previous->second, begin + node, graph::EdgeKind::Synchronization });
    }
    for (std::size_t id = 0; id < has_input.size(); ++id)
        if (cause && !has_input[id]) result.dependencies.push_back({ *cause, begin + id, graph::EdgeKind::DataDependency });

    std::optional<graph::NodeId> caller_submitted;
    if (events.produced) result.operations.at(result_.value()).completion = events.produced;
    if (events.submitted) {
        caller_submitted = begin + result.operations.size();
        result.operations.push_back({ caller_ ? steps_[*caller_].resource : 0, Boundary{}, *events.submitted });
        if (caller_) result.dependencies.push_back({ begin + *caller_, *caller_submitted, graph::EdgeKind::DataDependency });
        else if (cause) result.dependencies.push_back({ *cause, *caller_submitted, graph::EdgeKind::DataDependency });
    }
    const auto end = begin + result.operations.size();
    result.operations.push_back({ 0, Boundary{}, events.complete });
    for (const auto tail : { caller_, worker_, device_ ? std::optional(device_->completion) : std::nullopt })
        if (tail) result.dependencies.push_back({ begin + *tail, end, graph::EdgeKind::DataDependency });
    if (steps_.empty() && cause) result.dependencies.push_back({ *cause, end, graph::EdgeKind::DataDependency });
    if (caller_submitted) result.dependencies.push_back({ *caller_submitted, end, graph::EdgeKind::DataDependency });
    return result;
}

} // namespace markov::trace_graph::planning
