#include "markov/trace_graph/source/details.hpp"
#include "markov/trace_graph/source/native_queues.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
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
    return same_worker_work(before, after) && before.work == after.work && before.launches == after.launches && before.caller_work == after.caller_work
           && before.caller_preparations == after.caller_preparations;
}

bool same_caller_step(const planning::NativeCall & before, const planning::NativeCall & after) {
    if (!before.caller_work || !after.caller_work) return same_work(before, after);
    return before.kind == after.kind && before.caller_work->kind == after.caller_work->kind && before.preparation == after.preparation
           && before.synchronization == after.synchronization && before.queue_name == after.queue_name && before.incoming_stream == after.incoming_stream
           && std::ranges::equal(before.caller_preparations, after.caller_preparations, [](const auto & left, const auto & right) {
                  return left.work.kind == right.work.kind && left.observation_name == right.observation_name;
              });
}

bool sources_complete(const planning::NativeCall & call) {
    if (call.kind == planning::CallKind::HostCall) return true;
    if (call.kind == planning::CallKind::Cpu || call.kind == planning::CallKind::HostResponse) return call.sources.caller_before.has_value();
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

void SourceDetails::bind_native_sources(Index sequence, std::span<planning::NativeCall> calls, planning::CallResources resources) const {
    for (auto & call : calls) {
        call.sources = {};
        call.sources.launches.resize(call.launches.size());
    }
    const auto & selected = sequences_[sequence];
    const auto & binding = selected.native;
    if (!binding.issue && std::ranges::equal(binding.definitions, calls, same_work)) {
        for (Index id = 0; id < calls.size(); ++id) calls[id].sources = binding.definitions[id].sources;
    }
    else if (!binding.issue) {
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
    const auto & original = selected.observation;
    for (auto & call : calls) {
        call.sources.caller_preparations.resize(call.caller_preparations.size());
        for (Index id = 0; id < call.caller_preparations.size(); ++id) {
            const auto found = control_sources_.find({ resources.caller, call.caller_preparations[id].work });
            if (found != control_sources_.end()) call.sources.caller_preparations[id] = found->second;
        }
    }
    for (auto & call : calls) {
        if (call.device_control == planning::DeviceControl::None || call.launches.size() != 1) continue;
        const auto found = control_sources_.find({ resources.caller, call.launches.front() });
        if (found != control_sources_.end()) call.sources.launches.front().device = found->second;
    }
    for (const auto & site : sequences_) {
        if (std::ranges::all_of(calls, sources_complete)) break;
        const auto & candidate = site.native;
        const auto & owner = site.observation;
        if (candidate.issue || owner.region.resource != resources.caller) continue;

        // A fragment can remain unchanged when another step in the same
        // ordered caller recipe changes its work.
        if (owner.role == original.role && std::ranges::equal(candidate.definitions, calls, same_caller_step)) {
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
                // A worker body is independent of its framework caller recipe,
                // but must retain its declared work and execution resource.
                const auto bind_worker = [&](std::optional<Index> & target, std::optional<Index> from) {
                    if (!target && from && native_work_[*from].region.resource == resources.worker) target = from;
                };
                bind_worker(call.sources.dispatch, source.sources.dispatch);
                bind_worker(call.sources.worker_before, source.sources.worker_before);
                for (Index launch = 0; launch < call.launches.size(); ++launch) {
                    bind_worker(call.sources.launches[launch].worker_before, source.sources.launches[launch].worker_before);
                    if (owner.role == original.role && !call.sources.launches[launch].device && call.launches[launch] == source.launches[launch])
                        call.sources.launches[launch].device = source.sources.launches[launch].device;
                }
                bind_worker(call.sources.worker_after, source.sources.worker_after);
            }
        }
    }
}

Index SourceDetails::retain_native_work(planning::Work work, const char * scope, ObservedRegion region, HostWait generated_wait, NativeTiming timing,
                                        Time excluded_queue_wait_ns, std::vector<ObservedRegion> excluded) {
    work.kind.timing_scope = scope;
    const auto id = native_work_.size();
    native_work_.push_back({ std::move(work), region, generated_wait, timing, excluded_queue_wait_ns, std::move(excluded) });
    return id;
}

void SourceDetails::index_native_work() {
    const auto & observed = observations_;
    const auto & queues = observed.native_queue_calls;

    // shortcut: source specialization caches stay live; record invalidation before supporting cache resets.
    std::map<std::pair<Index, planning::Work>, Time> first_prepared;
    std::set<std::string> caller_preparation_names;
    for (const auto & site : sequences_) {
        const auto & binding = site.native;
        for (Index id = 0; id < binding.definitions.size(); ++id) {
            for (const auto & preparation : binding.definitions[id].caller_preparations) caller_preparation_names.insert(preparation.observation_name);
            const auto & preparation = binding.definitions[id].preparation;
            if (!preparation) continue;
            const auto & call = queues[binding.calls[id]];
            const auto key = std::pair(call.submission.resource, preparation->specialization);
            const auto [at, inserted] = first_prepared.try_emplace(key, call.submission.begin_ns);
            if (!inserted) at->second = std::min(at->second, call.submission.begin_ns);
        }
    }
    std::vector<std::vector<Index>> caller_queues(observed.resources.size()), caller_preparation_spans(observed.resources.size());
    for (Index id = 0; id < queues.size(); ++id) caller_queues[queues[id].submission.resource].push_back(id);
    const auto submitted = [&](Index id) { return queues[id].submission.begin_ns; };
    for (auto & lane : caller_queues) std::ranges::sort(lane, {}, submitted);
    std::vector<CallerEnvelope> parents(queues.size());
    for (Index id = 0; id < observed.spans.size(); ++id) {
        const auto & span = observed.spans[id];
        if (caller_preparation_names.contains(observed.names[span.name])) caller_preparation_spans[span.resource].push_back(id);
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
    const auto preparation_begin = [&](Index id) { return observed.spans[id].begin_ns; };
    for (auto & lane : caller_preparation_spans) std::ranges::sort(lane, {}, preparation_begin);
    const NativeQueues native_queues(observed, graph_);

    for (auto & site : sequences_) {
        auto & binding = site.native;
        if (binding.issue) continue;
        const auto & region = site.observation.region;
        if (binding.definitions.size() == 1
            && (binding.definitions.front().kind == planning::CallKind::Cpu || binding.definitions.front().kind == planning::CallKind::HostResponse)) {
            auto & definition = binding.definitions.front();
            const auto * result = source_.frame_result(site.frame);
            const bool consumes_result = site.result_processing && result && result->processed && region.begin_ns == result->processed->region.begin_ns;
            definition.sources.caller_before = retain_native_work(definition.work,
                                                                  "cpu_service",
                                                                  region,
                                                                  definition.kind == planning::CallKind::HostResponse ? HostWait::Stream
                                                                  : consumes_result                                   ? HostWait::ResultEvent
                                                                                                                      : HostWait::None);
            if (consumes_result) {
                std::optional<Time> producer;
                for (const auto id : frame_sequences_[site.frame]) {
                    const auto & candidate = sequences_[id].native;
                    if (candidate.issue || candidate.result_ready != planning::NativeProducer::Device || candidate.definitions.empty()) continue;
                    const auto & tail = candidate.definitions.back().sources.launches;
                    if (!tail.empty() && tail.back().device) producer = native_work_[*tail.back().device].region.end_ns;
                }
                for (const auto & preparation : definition.caller_preparations) {
                    if (!producer) continue;
                    const auto & lane = caller_preparation_spans[region.resource];
                    for (auto at = std::ranges::lower_bound(lane, region.begin_ns, {}, preparation_begin);
                         at != lane.end() && preparation_begin(*at) < region.end_ns;
                         ++at) {
                        const auto & span = observed.spans[*at];
                        if (span.resource != region.resource || span.begin_ns < region.begin_ns || region.end_ns < span.end_ns
                            || observed.names[span.name] != preparation.observation_name || span.role != SpanRole::EventWait || *producer > span.begin_ns)
                            continue;
                        const auto id = retain_native_work(preparation.work,
                                                           preparation.work.kind.timing_scope.c_str(),
                                                           { span.resource, span.begin_ns, span.end_ns, span.record },
                                                           HostWait::None,
                                                           NativeTiming::DeviceControl);
                        control_sources_.try_emplace(std::pair(region.resource, preparation.work), id);
                    }
                }
            }
            continue;
        }
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
            const auto ports = native_queues.ports(binding.calls[id], definition);
            if (ports.dispatch)
                sources.dispatch = retain_native_work(definition.worker_work(definition.work),
                                                      "dispatch_interval",
                                                      *ports.dispatch,
                                                      HostWait::None,
                                                      NativeTiming::ReadyDispatch,
                                                      ports.excluded_queue_wait_ns);
            if (caller_partition) {
                const auto & parent = *parents[binding.calls[id]].region;
                auto work = definition.caller_work ? *definition.caller_work : definition.work;

                // A prior submission proves the declared specialization is
                // prepared. The first opaque compile/load prelude stays out.
                const auto prepared = !definition.preparation || first_prepared.at({ region.resource, definition.preparation->specialization }) < cursor;
                std::vector<ObservedRegion> excluded;
                bool preparation_complete = true;
                if (!definition.caller_preparations.empty()) {
                    const auto & lane = caller_preparation_spans[region.resource];
                    for (auto at = std::ranges::lower_bound(lane, cursor, {}, preparation_begin);
                         at != lane.end() && preparation_begin(*at) < call.submission.begin_ns;
                         ++at) {
                        const auto & span = observed.spans[*at];
                        if (span.record.file != parent.record.file) continue;
                        if (excluded.size() == definition.caller_preparations.size() || span.end_ns > call.submission.begin_ns
                            || (!excluded.empty() && span.begin_ns < excluded.back().end_ns)
                            || observed.names[span.name] != definition.caller_preparations[excluded.size()].observation_name) {
                            preparation_complete = false;
                            break;
                        }
                        excluded.push_back({ span.resource, span.begin_ns, span.end_ns, span.record });
                    }
                    preparation_complete = preparation_complete && excluded.size() == definition.caller_preparations.size();
                }
                if (prepared && preparation_complete)
                    sources.caller_before = retain_native_work(work,
                                                               "cpu_service",
                                                               { region.resource, cursor, call.submission.begin_ns, parent.record },
                                                               HostWait::None,
                                                               NativeTiming::Cpu,
                                                               0,
                                                               std::move(excluded));
                work.kind.operation += ".return";
                const auto end = id + 1 == binding.calls.size() ? region.end_ns : parent.end_ns;
                sources.caller_after = retain_native_work(
                    std::move(work),
                    "cpu_service",
                    { region.resource, call.submission.begin_ns, end, parent.record },
                    definition.kind == planning::CallKind::HostUpload && definition.synchronization.has_value() ? HostWait::Stream : HostWait::None);
                cursor = parent.end_ns;
            }

            if (ports.worker_before)
                sources.worker_before = retain_native_work(definition.worker_work(definition.work), "worker_launch_service", *ports.worker_before);
            for (Index launch = 0; launch < ports.launches.size(); ++launch) {
                const auto & observed_launch = ports.launches[launch];
                const auto & work = definition.launches[launch];
                auto & source = sources.launches[launch];
                if (observed_launch.worker_before)
                    source.worker_before = retain_native_work(definition.worker_work(work), "worker_launch_service", *observed_launch.worker_before);
                if (!observed_launch.device) continue;
                const bool control = definition.device_control != planning::DeviceControl::None;
                source.device = retain_native_work(work,
                                                   "device_service",
                                                   *observed_launch.device,
                                                   HostWait::None,
                                                   control ? NativeTiming::DeviceControl : NativeTiming::DeviceService);
                if (control) control_sources_.try_emplace(std::pair(region.resource, work), *source.device);
            }
            if (ports.worker_after)
                sources.worker_after = retain_native_work(definition.worker_work(definition.work), "worker_return_service", *ports.worker_after);
        }
    }
}

} // namespace markov::trace_graph::source
