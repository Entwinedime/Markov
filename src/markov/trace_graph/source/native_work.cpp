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

bool same_worker_work(const planning::NativeCall & before, const planning::NativeCall & after) {
    if (before.kind != after.kind || before.synchronization != after.synchronization || before.host_transfer.has_value() != after.host_transfer.has_value()
        || before.preparation != after.preparation || before.incoming_stream != after.incoming_stream || before.queue_name != after.queue_name
        || before.device_control != after.device_control || before.worker_first_use != after.worker_first_use)
        return false;
    if (before.host_transfer && before.host_transfer->kind != after.host_transfer->kind) return false;

    // Explicit worker variables exclude the independently priced transfer
    // payload; default descriptors retain the full transfer condition.
    if ((!before.worker_features || !after.worker_features) && before.host_transfer != after.host_transfer) return false;

    const auto same_submission = [&](const planning::Work & left, const planning::Work & right) {
        const auto & left_features = before.worker_features ? *before.worker_features : left.features;
        const auto & right_features = after.worker_features ? *after.worker_features : right.features;
        return left.kind == right.kind && left_features == right_features;
    };
    return same_submission(before.work, after.work) && std::ranges::equal(before.launches, after.launches, same_submission);
}

bool same_work(const planning::NativeCall & before, const planning::NativeCall & after) {
    return same_worker_work(before, after) && before.work == after.work && before.launches == after.launches && before.caller_work == after.caller_work;
}

bool same_caller_step(const planning::NativeCall & before, const planning::NativeCall & after) {
    if (!before.caller_work || !after.caller_work) return same_work(before, after);
    return before.kind == after.kind && before.caller_work->kind == after.caller_work->kind && before.preparation == after.preparation
           && before.synchronization == after.synchronization && before.queue_name == after.queue_name && before.incoming_stream == after.incoming_stream;
}

bool sources_complete(const planning::NativeCall & call) {
    if (!call.sources.caller_before || !call.sources.caller_after || !call.sources.dispatch || !call.sources.worker_after) return false;
    if (call.kind == planning::CallKind::HostUpload) return call.sources.worker_before.has_value();
    return std::ranges::all_of(call.sources.launches, [](const planning::NativeLaunchSources & sources) { return sources.worker_before.has_value(); });
}

struct CallerEnvelope {
    std::optional<ObservedRegion> region;
    bool exact = false;

    // An observed outer span must cover the candidates; overlap alone is not a return port.
    void include(const ObservedSpan & span) {
        const auto begin = region ? std::min(region->begin_ns, span.begin_ns) : span.begin_ns;
        const auto end = region ? std::max(region->end_ns, span.end_ns) : span.end_ns;
        if (span.begin_ns == begin && span.end_ns == end) {
            region = ObservedRegion{ span.resource, begin, end, span.record };
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

void SourceContext::bind_native_sources(Index frame, Index preparation, std::span<planning::NativeCall> calls) const {
    for (auto & call : calls) {
        call.sources = {};
        call.sources.launches.resize(call.launches.size());
    }
    const auto & binding = preparation_native_calls_[frame][preparation];
    if (binding.issue) return;
    if (std::ranges::equal(binding.definitions, calls, same_work)) {
        for (Index id = 0; id < calls.size(); ++id) calls[id].sources = binding.definitions[id].sources;
    }
    else {
        // Caller fragments include the surrounding framework prelude. A worker
        // body can remain unchanged when that prelude or another call changes.
        for (auto & call : calls) {
            const planning::NativeCall * match = nullptr;
            bool ambiguous = false;
            for (const auto & original : binding.definitions) {
                if (!same_worker_work(original, call)) continue;
                if (match) ambiguous = true;
                match = &original;
            }
            if (!match || ambiguous) continue;
            call.sources.worker_before = match->sources.worker_before;
            call.sources.worker_after = match->sources.worker_after;
            call.sources.dispatch = match->sources.dispatch;
            for (Index id = 0; id < call.launches.size(); ++id) {
                call.sources.launches[id].worker_before = match->sources.launches[id].worker_before;
                if (call.launches[id] == match->launches[id]) call.sources.launches[id].device = match->sources.launches[id].device;
            }
        }
    }

    // Another invocation can supply ordinary work missing from this callback;
    // caller recipes and worker bodies retain separate matching conditions.
    const auto & original = observations_.compute_frames[frame].preparations[preparation];
    for (auto & call : calls) {
        if (call.device_control == planning::DeviceControl::None || call.launches.size() != 1) continue;
        const auto found = control_sources_.find({ original.region.resource, call.launches.front() });
        if (found != control_sources_.end()) call.sources.launches.front().device = found->second;
    }
    for (Index other_frame = 0; other_frame < preparation_native_calls_.size(); ++other_frame) {
        if (std::ranges::all_of(calls, sources_complete)) break;
        for (Index id = 0; id < preparation_native_calls_[other_frame].size(); ++id) {
            const auto & candidate = preparation_native_calls_[other_frame][id];
            const auto & owner = observations_.compute_frames[other_frame].preparations[id];
            if (candidate.issue || owner.region.resource != original.region.resource || owner.role != original.role) continue;

            // A fragment can remain unchanged when another step in the same
            // ordered caller recipe changes its work.
            if (std::ranges::equal(candidate.definitions, calls, same_caller_step)) {
                for (Index call = 0; call < calls.size(); ++call) {
                    if (calls[call].caller_work != candidate.definitions[call].caller_work) continue;
                    auto & sources = calls[call].sources;
                    if (!sources.caller_before) sources.caller_before = candidate.definitions[call].sources.caller_before;
                    if (!sources.caller_after) sources.caller_after = candidate.definitions[call].sources.caller_after;
                }
            }
            for (auto & call : calls) {
                for (const auto & source : candidate.definitions) {
                    if (!same_worker_work(source, call)) continue;
                    if (!call.sources.dispatch) call.sources.dispatch = source.sources.dispatch;
                    if (!call.sources.worker_before) call.sources.worker_before = source.sources.worker_before;
                    for (Index launch = 0; launch < call.launches.size(); ++launch) {
                        if (!call.sources.launches[launch].worker_before)
                            call.sources.launches[launch].worker_before = source.sources.launches[launch].worker_before;
                        if (!call.sources.launches[launch].device && call.launches[launch] == source.launches[launch])
                            call.sources.launches[launch].device = source.sources.launches[launch].device;
                    }
                    if (!call.sources.worker_after) call.sources.worker_after = source.sources.worker_after;
                }
            }
        }
    }
}

Index SourceContext::retain_native_work(planning::Work work, const char * scope, ObservedRegion region, bool stream_wait_generated, NativeTiming timing,
                                        Time excluded_queue_wait_ns) {
    work.kind.timing_scope = scope;
    const auto id = native_work_.size();
    native_work_.push_back({ std::move(work), region, stream_wait_generated, timing, excluded_queue_wait_ns });
    return id;
}

void SourceContext::index_native_work() {
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
    std::vector<std::vector<Index>> caller_queues(observed.resources.size()), worker_queues(observed.resources.size()),
        runtime_calls(observed.resources.size()), submissions(observed.resources.size()), controls(observed.resources.size());
    for (Index id = 0; id < queues.size(); ++id) {
        caller_queues[queues[id].submission.resource].push_back(id);
        worker_queues[queues[id].execution.resource].push_back(id);
    }
    const auto executed = [&](Index id) { return queues[id].execution.begin_ns; };
    std::vector<std::optional<Time>> previous_worker_end(queues.size());
    for (auto & lane : worker_queues) {
        std::ranges::sort(lane, {}, executed);
        for (Index id = 1; id < lane.size(); ++id) {
            const auto & before = queues[lane[id - 1]].execution;
            const auto & after = queues[lane[id]].execution;
            if (before.end_ns <= after.begin_ns && before.begin_ns < after.begin_ns) previous_worker_end[lane[id]] = before.end_ns;
        }
    }
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
    for (Index id = 0; id < observed.device_controls.size(); ++id) controls[observed.device_controls[id].submitted.resource].push_back(id);
    const auto control_begin = [&](Index id) { return observed.device_controls[id].submitted.begin_ns; };
    for (auto & lane : controls) std::ranges::sort(lane, {}, control_begin);

    std::vector<Time> exclusive_service(observed.spans.size(), 0);
    for (graph::NodeId id = 0; id < graph_.graph.nodes().size(); ++id) {
        const auto & node = graph_.graph.nodes()[id];
        if (node.kind == graph::NodeKind::Service) exclusive_service[graph_.node_origins[id].span] += node.duration_ns;
    }
    std::map<std::pair<Index, Time>, std::optional<Index>> device_spans;
    for (Index id = 0; id < observed.spans.size(); ++id) {
        const auto & span = observed.spans[id];
        if (observed.resources[span.resource].kind != ResourceKind::DeviceStream || span.begin_ns == span.end_ns) continue;
        const auto [at, inserted] = device_spans.try_emplace({ span.resource, span.begin_ns }, id);
        if (!inserted) at->second.reset();
    }

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
                auto & sources = definition.sources;
                sources.launches.resize(definition.launches.size());
                const auto & call = queues[binding.calls[id]];
                if (previous_worker_end[binding.calls[id]]) {
                    const auto ready = std::max(call.submission.begin_ns, *previous_worker_end[binding.calls[id]]);
                    sources.dispatch = retain_native_work(definition.worker_work(definition.work),
                                                          "dispatch_interval",
                                                          { call.execution.resource, ready, call.execution.begin_ns, call.execution.record },
                                                          false,
                                                          NativeTiming::ReadyDispatch,
                                                          ready - call.submission.begin_ns);
                }
                if (caller_partition) {
                    const auto & parent = *parents[binding.calls[id]].region;
                    auto work = definition.caller_work ? *definition.caller_work : definition.work;

                    // A prior submission proves the declared specialization is
                    // prepared. The first opaque compile/load prelude stays out.
                    const auto prepared = !definition.preparation || first_prepared.at({ region.resource, definition.preparation->specialization }) < cursor;
                    if (prepared)
                        sources.caller_before = retain_native_work(work, "cpu_service", { region.resource, cursor, call.submission.begin_ns, parent.record });
                    work.kind.operation += ".return";
                    const auto end = id + 1 == binding.calls.size() ? region.end_ns : parent.end_ns;
                    sources.caller_after = retain_native_work(std::move(work),
                                                              "cpu_service",
                                                              { region.resource, call.submission.begin_ns, end, parent.record },
                                                              definition.kind == planning::CallKind::HostUpload && definition.synchronization.has_value());
                    cursor = parent.end_ns;
                }

                const auto & execution = call.execution;
                if (definition.device_control != planning::DeviceControl::None && definition.launches.size() == 1) {
                    const auto & lane = controls[execution.resource];
                    const ObservedDeviceControl * command = nullptr;
                    bool ambiguous = false;
                    for (auto at = std::ranges::lower_bound(lane, execution.begin_ns, {}, control_begin);
                         at != lane.end() && control_begin(*at) < execution.end_ns;
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
                    if (ready && !ambiguous) {
                        sources.launches.front().device =
                            retain_native_work(definition.launches.front(), "device_service", command->command, false, NativeTiming::DeviceControl);
                        control_sources_.try_emplace({ region.resource, definition.launches.front() }, *sources.launches.front().device);
                    }
                }
                if (definition.kind == planning::CallKind::HostUpload) {
                    const auto & lane = runtime_calls[execution.resource];
                    std::optional<ObservedRegion> copy;
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
                    sources.worker_before = retain_native_work(definition.worker_work(definition.work),
                                                               "worker_launch_service",
                                                               { execution.resource, execution.begin_ns, copy->begin_ns, execution.record });
                    sources.worker_after = retain_native_work(definition.worker_work(definition.work),
                                                              "worker_return_service",
                                                              { execution.resource, copy->end_ns, execution.end_ns, execution.record });
                }
                else if (definition.launches.empty())
                    sources.worker_after = retain_native_work(definition.worker_work(definition.work), "worker_return_service", execution);
                else {
                    const auto & lane = submissions[execution.resource];
                    std::map<std::pair<Time, Index>, std::set<Index>> ports;
                    std::set<std::pair<Time, Index>> unresolved;
                    for (auto at = std::ranges::lower_bound(lane, execution.begin_ns, {}, submitted_device);
                         at != lane.end() && submitted_device(*at) <= execution.end_ns;
                         ++at) {
                        const auto & edge = observed.dependencies[*at];
                        const auto & target = observed.points[edge.after];
                        const auto key = std::pair(submitted_device(*at), target.resource);
                        auto & launched = ports[key];
                        const auto span = device_spans.find({ target.resource, target.time_ns });
                        if (span == device_spans.end() || !span->second || submitted_device(*at) == execution.end_ns) unresolved.insert(key);
                        else launched.insert(*span->second);
                    }
                    if (ports.size() != definition.launches.size()) continue;

                    // An exported Node@launch port may precede binary lookup.
                    // Unique native APIs in this worker provide ordered entry ports.
                    const auto & runtime_lane = runtime_calls[execution.resource];
                    std::vector<Time> api_ports;
                    bool code_preparation = false;
                    for (auto at = std::ranges::lower_bound(runtime_lane, execution.begin_ns, {}, runtime_begin);
                         at != runtime_lane.end() && runtime_begin(*at) < execution.end_ns;
                         ++at) {
                        const auto & runtime = observed.native_runtime_calls[*at];
                        if (runtime.region.record.file != execution.record.file || runtime.region.end_ns > execution.end_ns) continue;
                        if (runtime.device_submission) api_ports.push_back(runtime.region.begin_ns);
                        code_preparation = code_preparation || runtime.code_preparation;
                    }
                    const bool first_use = definition.worker_first_use.has_value();
                    if (first_use && api_ports.size() != ports.size()) continue;

                    // A loading callback is not ordinary worker evidence,
                    // including its unseparated initialization outside the APIs.
                    const bool ordinary_worker = !first_use || !code_preparation;
                    auto cursor = execution.begin_ns;
                    Index launch = 0;
                    for (const auto & [port, spans] : ports) {
                        const auto & work = definition.launches[launch];
                        auto & source = sources.launches[launch];
                        const auto submitted = api_ports.size() == ports.size() ? api_ports[launch] : port.first;
                        if (ordinary_worker)
                            source.worker_before = retain_native_work(definition.worker_work(work),
                                                                      "worker_launch_service",
                                                                      { execution.resource, cursor, submitted, execution.record });

                        // A parent envelope or unresolved task cannot supply
                        // a complete exclusive device service.
                        if (definition.device_control == planning::DeviceControl::None && !unresolved.contains(port) && spans.size() == 1) {
                            const auto id = *spans.begin();
                            const auto & span = observed.spans[id];
                            if (exclusive_service[id] == span.end_ns - span.begin_ns)
                                source.device = retain_native_work(work,
                                                                   "device_service",
                                                                   { span.resource, span.begin_ns, span.end_ns, span.record },
                                                                   false,
                                                                   NativeTiming::DeviceService);
                        }
                        cursor = submitted;
                        ++launch;
                    }
                    if (ordinary_worker)
                        sources.worker_after = retain_native_work(definition.worker_work(definition.work),
                                                                  "worker_return_service",
                                                                  { execution.resource, cursor, execution.end_ns, execution.record });
                }
            }
        }
    }
}

} // namespace markov::trace_graph::source
