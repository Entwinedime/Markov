#include "markov/trace_graph/source/context.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace markov::trace_graph::source {
namespace {

bool same_native_work(const planning::NativeCall & before, const planning::NativeCall & after) {
    return before.kind == after.kind && before.work == after.work && before.launches == after.launches && before.synchronization == after.synchronization
           && before.host_transfer == after.host_transfer && before.preparation == after.preparation && before.incoming_stream == after.incoming_stream
           && before.queue_name == after.queue_name;
}

bool same_work(const planning::NativeCall & before, const planning::NativeCall & after) {
    return same_native_work(before, after) && before.caller_work == after.caller_work;
}

struct CallerEnvelope {
    std::optional<CpuRegion> region;
    bool exact = false;

    // An observed outer span must cover the candidates; overlap alone is not a return port.
    void include(const ObservedSpan & span) {
        const auto begin = region ? std::min(region->begin_ns, span.begin_ns) : span.begin_ns;
        const auto end = region ? std::max(region->end_ns, span.end_ns) : span.end_ns;
        if (span.begin_ns == begin && span.end_ns == end) {
            region = CpuRegion{ span.resource, begin, end, span.record };
            exact = true;
        }
        else if (begin != region->begin_ns || end != region->end_ns) {
            region->begin_ns = begin;
            region->end_ns = end;
            exact = false;
        }
    }
};

} // namespace

void SourceContext::bind_native_cpu_sources(Index frame, Index preparation, std::span<planning::NativeCall> calls) const {
    for (auto & call : calls) call.source_cpu = {};
    const auto & binding = preparation_native_calls_[frame][preparation];
    if (binding.issue) return;
    if (std::ranges::equal(binding.definitions, calls, same_work)) {
        for (Index id = 0; id < calls.size(); ++id) calls[id].source_cpu = binding.definitions[id].source_cpu;
    }
    else {
        // Caller fragments include the surrounding framework prelude. A worker
        // body can remain unchanged when that prelude or another call changes.
        for (auto & call : calls) {
            const planning::NativeCall * match = nullptr;
            bool ambiguous = false;
            for (const auto & original : binding.definitions) {
                if (!same_native_work(original, call)) continue;
                if (match) ambiguous = true;
                match = &original;
            }
            if (!match || ambiguous) continue;
            call.source_cpu.worker_before = match->source_cpu.worker_before;
            call.source_cpu.worker_after = match->source_cpu.worker_after;
        }
    }

    // Another invocation in the same base can supply a prepared prelude that
    // the first invocation could not separate from compilation and loading.
    const auto & original = observations_.compute_frames[frame].preparations[preparation];
    for (Index other_frame = 0; other_frame < preparation_native_calls_.size(); ++other_frame) {
        if (std::ranges::all_of(calls, [](const planning::NativeCall & call) { return call.source_cpu.caller_before && call.source_cpu.caller_after; })) break;
        for (Index id = 0; id < preparation_native_calls_[other_frame].size(); ++id) {
            const auto & candidate = preparation_native_calls_[other_frame][id];
            const auto & owner = observations_.compute_frames[other_frame].preparations[id];
            if (candidate.issue || owner.region.resource != original.region.resource || owner.role != original.role
                || !std::ranges::equal(candidate.definitions, calls, same_work))
                continue;
            for (Index call = 0; call < calls.size(); ++call) {
                auto & sources = calls[call].source_cpu;
                if (!sources.caller_before) sources.caller_before = candidate.definitions[call].source_cpu.caller_before;
                if (!sources.caller_after) sources.caller_after = candidate.definitions[call].source_cpu.caller_after;
            }
        }
    }
}

Index SourceContext::retain_cpu_work(planning::Work work, const char * scope, CpuRegion region, bool stream_wait_generated) {
    work.kind.timing_scope = scope;
    const auto id = native_cpu_work_.size();
    native_cpu_work_.push_back({ std::move(work), region, stream_wait_generated });
    return id;
}

void SourceContext::index_native_cpu_work() {
    const auto & observed = observations_;
    const auto & queues = observed.native_queue_calls;

    // shortcut: source specialization caches stay live; record invalidation before supporting cache resets.
    std::map<std::pair<Index, planning::Work>, Time> first_prepared;
    for (const auto & frame : preparation_native_calls_) {
        for (const auto & binding : frame) {
            for (Index id = 0; id < binding.definitions.size(); ++id) {
                const auto & preparation = binding.definitions[id].preparation;
                if (!preparation) continue;
                const auto & call = queues[binding.calls[id]];
                const auto key = std::pair(call.submission.resource, preparation->specialization);
                const auto [at, inserted] = first_prepared.try_emplace(key, call.submission.begin_ns);
                if (!inserted) at->second = std::min(at->second, call.submission.begin_ns);
            }
        }
    }
    std::vector<std::vector<Index>> caller_queues(observed.resources.size()), runtime_calls(observed.resources.size()), submissions(observed.resources.size());
    for (Index id = 0; id < queues.size(); ++id) caller_queues[queues[id].submission.resource].push_back(id);
    const auto submitted = [&](Index id) { return queues[id].submission.begin_ns; };
    for (auto & lane : caller_queues) std::ranges::sort(lane, {}, submitted);
    std::vector<CallerEnvelope> parents(queues.size());
    for (const auto & span : observed.spans) {
        const auto & lane = caller_queues[span.resource];
        const auto at = std::ranges::lower_bound(lane, span.begin_ns, {}, submitted);
        if (at == lane.end()) continue;
        const auto & call = queues[*at];
        const auto next = std::next(at);
        if (span.record.file != call.submission.record.file || span.record.position == call.submission.record.position || span.end_ns < call.submission.end_ns
            || (next != lane.end() && submitted(*next) < span.end_ns))
            continue;
        parents[*at].include(span);
    }
    for (Index id = 0; id < observed.native_runtime_calls.size(); ++id) runtime_calls[observed.native_runtime_calls[id].region.resource].push_back(id);
    const auto runtime_begin = [&](Index id) { return observed.native_runtime_calls[id].region.begin_ns; };
    for (auto & lane : runtime_calls) std::ranges::sort(lane, {}, runtime_begin);
    for (Index id = 0; id < observed.dependencies.size(); ++id) {
        const auto & edge = observed.dependencies[id];
        if (edge.kind != DependencyKind::Synchronization && observed.resources[observed.points[edge.after].resource].kind == ResourceKind::DeviceStream)
            submissions[observed.points[edge.before].resource].push_back(id);
    }
    const auto submitted_device = [&](Index id) { return observed.points[observed.dependencies[id].before].time_ns; };
    for (auto & lane : submissions) std::ranges::sort(lane, {}, submitted_device);

    for (Index frame = 0; frame < preparation_native_calls_.size(); ++frame) {
        for (Index preparation = 0; preparation < preparation_native_calls_[frame].size(); ++preparation) {
            auto & binding = preparation_native_calls_[frame][preparation];
            if (binding.issue) continue;
            const auto & region = observed.compute_frames[frame].preparations[preparation].region;
            auto cursor = region.begin_ns;
            bool caller_partition = true;
            for (const auto id : binding.calls) {
                const auto & parent = parents[id];
                if (!parent.exact || parent.region->begin_ns < cursor || parent.region->end_ns > region.end_ns) {
                    caller_partition = false;
                    break;
                }
                cursor = parent.region->end_ns;
            }
            cursor = region.begin_ns;
            for (Index id = 0; id < binding.calls.size(); ++id) {
                auto & definition = binding.definitions[id];
                auto & sources = definition.source_cpu;
                const auto & call = queues[binding.calls[id]];
                if (caller_partition) {
                    const auto & parent = *parents[binding.calls[id]].region;
                    auto work = definition.caller_work ? *definition.caller_work : definition.work;

                    // A prior submission proves the declared specialization is
                    // prepared. The first opaque compile/load prelude stays out.
                    const auto prepared = !definition.preparation || first_prepared.at({ region.resource, definition.preparation->specialization }) < cursor;
                    if (prepared)
                        sources.caller_before = retain_cpu_work(work, "cpu_service", { region.resource, cursor, call.submission.begin_ns, parent.record });
                    work.kind.operation += ".return";
                    const auto end = id + 1 == binding.calls.size() ? region.end_ns : parent.end_ns;
                    sources.caller_after = retain_cpu_work(std::move(work),
                                                           "cpu_service",
                                                           { region.resource, call.submission.begin_ns, end, parent.record },
                                                           definition.kind == planning::CallKind::HostUpload && definition.synchronization.has_value());
                    cursor = parent.end_ns;
                }

                const auto & execution = call.execution;
                if (definition.kind == planning::CallKind::HostUpload) {
                    const auto & lane = runtime_calls[execution.resource];
                    std::optional<CpuRegion> copy;
                    bool ambiguous = false;
                    for (auto at = std::ranges::lower_bound(lane, execution.begin_ns, {}, runtime_begin);
                         at != lane.end() && runtime_begin(*at) < execution.end_ns;
                         ++at) {
                        const auto & runtime = observed.native_runtime_calls[*at];
                        if (runtime.region.record.file != execution.record.file || runtime.region.end_ns > execution.end_ns
                            || runtime.region.begin_ns == runtime.region.end_ns
                            || observed.names[runtime.implementation] != definition.host_transfer->kind.implementation)
                            continue;
                        if (copy) ambiguous = true;
                        copy = runtime.region;
                    }
                    if (!copy || ambiguous) continue;
                    sources.worker_before =
                        retain_cpu_work(definition.work, "worker_launch_service", { execution.resource, execution.begin_ns, copy->begin_ns, execution.record });
                    sources.worker_after =
                        retain_cpu_work(definition.work, "worker_return_service", { execution.resource, copy->end_ns, execution.end_ns, execution.record });
                }
                else if (definition.launches.empty()) sources.worker_after = retain_cpu_work(definition.work, "worker_return_service", execution);
                else if (definition.launches.size() == 1) {
                    const auto & lane = submissions[execution.resource];
                    std::set<std::pair<Time, Index>> ports;
                    for (auto at = std::ranges::lower_bound(lane, execution.begin_ns, {}, submitted_device);
                         at != lane.end() && submitted_device(*at) <= execution.end_ns;
                         ++at) {
                        const auto & edge = observed.dependencies[*at];
                        ports.emplace(observed.points[edge.before].time_ns, observed.points[edge.after].resource);
                    }
                    if (ports.size() != 1) continue;
                    const auto submitted = ports.begin()->first;
                    sources.worker_before = retain_cpu_work(definition.launches.front(),
                                                            "worker_launch_service",
                                                            { execution.resource, execution.begin_ns, submitted, execution.record });
                    sources.worker_after =
                        retain_cpu_work(definition.work, "worker_return_service", { execution.resource, submitted, execution.end_ns, execution.record });
                }
            }
        }
    }
}

} // namespace markov::trace_graph::source
