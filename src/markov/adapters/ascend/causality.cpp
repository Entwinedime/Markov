#include "markov/adapters/ascend/causality.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <utility>

namespace markov::adapters::ascend {

using namespace trace_graph::source;

std::optional<RuntimeCausality::Kind> RuntimeCausality::host_kind(const std::string & name) {
    if (name == "AscendCL@aclrtRecordEvent") return Kind::Record;
    if (name == "AscendCL@aclrtStreamWaitEvent") return Kind::Wait;
    if (name == "AscendCL@aclrtSynchronizeEvent" || name == "AscendCL@aclrtSynchronizeEventWithTimeout") return Kind::EventSynchronize;
    if (name == "AscendCL@aclrtSynchronizeStream" || name == "AscendCL@aclrtSynchronizeStreamWithTimeout") return Kind::StreamSynchronize;
    return std::nullopt;
}

void RuntimeCausality::observe(const nlohmann::json & event, ObservedSpan & span, bool device, bool hook) {
    const auto & name = event.at("name").get_ref<const std::string &>();
    if (device && (name == "PROFILING_ENABLE" || name == "PROFILING_DISABLE")) span.role = SpanRole::ProfilerOverhead;
    if (name == "EVENT_WAIT" || name == "NOTIFY_WAIT" || name.starts_with("AscendCL@aclrtSynchronize") || name == "Runtime@StreamSynchronize"
        || name == "Runtime@EventSynchronize" || name == "Runtime@DeviceSynchronize")
        span.role = SpanRole::Wait;
    if (name.starts_with("AscendCL@aclrtSynchronizeStream") || name == "Runtime@StreamSynchronize") span.role = SpanRole::StreamWait;
    if (device) {
        const auto & args = event.at("args");
        if (name != "EVENT_RECORD" && name != "EVENT_WAIT") return;

        const auto kind = name == "EVENT_RECORD" ? Kind::Record : Kind::Wait;
        const auto connection = scalar_identity(args.at("connection_id"));
        device_calls_.push_back({ kind, span, {}, connection, {} });
        return;
    }
    const auto kind = host_kind(name);
    if (!kind) return;

    const auto & args = event.at("args");
    if (hook) {
        const auto & parameters = args.at("Function-Args");
        const auto identity = *kind == Kind::StreamSynchronize ? std::string{} : scalar_identity(parameters.at("event"));
        const auto stream = *kind == Kind::EventSynchronize ? std::string{} : scalar_identity(parameters.at("stream"));
        hook_calls_.push_back({ *kind, span, scalar_identity(event.at("pid")), identity, stream });
    }
    else host_calls_.push_back({ *kind, span, {}, scalar_identity(args.at("connection_id")), {} });
}

std::vector<RuntimeCausality::Binding> RuntimeCausality::bind_calls(Observations & observations) const {
    using CallKey = std::pair<Index, Index>;
    using Connection = std::pair<Index, std::string>;
    std::map<CallKey, std::vector<const Call *>> hosts;
    std::map<Connection, std::vector<const Call *>> devices;
    for (const auto & call : host_calls_) hosts[{ observations.canonical_resource(call.span.resource), call.span.name }].push_back(&call);
    for (auto & [key, calls] : hosts) std::ranges::sort(calls, [](const Call * a, const Call * b) { return a->span.begin_ns < b->span.begin_ns; });
    for (const auto & call : device_calls_) devices[{ call.span.record.file, call.identity }].push_back(&call);
    std::vector<Binding> bindings;
    bindings.reserve(hook_calls_.size());
    for (const auto & hook : hook_calls_) {
        const auto group = hosts.find({ hook.span.resource, hook.span.name });
        const Call * host = nullptr;
        bool ambiguous = false;
        if (group != hosts.end()) {
            const auto & calls = group->second;
            auto begin = std::ranges::lower_bound(calls, hook.span.begin_ns, {}, [](const Call * call) { return call->span.begin_ns; });
            if (begin != calls.begin()) --begin;
            for (auto at = begin; at != calls.end() && (*at)->span.begin_ns <= hook.span.end_ns; ++at) {
                if ((*at)->span.end_ns < hook.span.begin_ns) continue;

                if (host) {
                    ambiguous = true;
                    break;
                }
                host = *at;
            }
        }
        if (ambiguous) host = nullptr;
        if (!host) ++observations.counts[ambiguous ? "ascend.ambiguous_hook_call" : "ascend.hook_call_without_cann_observation"];

        const Call * device = nullptr;
        if (host && (hook.kind == Kind::Record || hook.kind == Kind::Wait)) {
            const auto found = devices.find({ host->span.record.file, host->identity });
            if (found != devices.end() && found->second.size() == 1 && found->second.front()->kind == hook.kind) device = found->second.front();
            else ++observations.counts["ascend.event_call_without_unique_device_command"];
        }
        bindings.push_back({ &hook, host, device });
    }
    return bindings;
}

void RuntimeCausality::connect_events(Observations & observations, const std::vector<Binding> & bindings) const {
    // Event handles are process-wide, not thread-local. Resource identity is
    // only used for hook/CANN matching; event reuse is scoped by native PID.
    std::map<std::pair<std::string, std::string>, std::vector<const Binding *>> events;
    for (const auto & binding : bindings) {
        if (binding.hook->kind == Kind::Record) events[{ binding.hook->process, binding.hook->identity }].push_back(&binding);
    }
    for (auto & [key, values] : events)
        std::ranges::sort(values, [](const Binding * a, const Binding * b) { return a->hook->span.begin_ns < b->hook->span.begin_ns; });

    for (const auto & binding : bindings) {
        const auto & hook = *binding.hook;
        std::optional<Index> control;
        // Connection IDs bind the actual command, including zero-duration
        // records whose independent exported flow port is only a sibling.
        if (binding.host && binding.device) {
            observations.dependencies.push_back(
                { port(observations, *binding.host, false), port(observations, *binding.device, false), DependencyKind::Submission });
            ++observations.counts["ascend.event_submissions"];
            const auto & submitted = binding.host->span;
            const auto & command = binding.device->span;
            control = observations.device_controls.size();
            observations.device_controls.push_back({
                hook.kind == Kind::Record ? trace_graph::planning::DeviceControl::Record : trace_graph::planning::DeviceControl::Wait,
                { observations.canonical_resource(submitted.resource), submitted.begin_ns, submitted.end_ns, submitted.record },
                {   observations.canonical_resource(command.resource),   command.begin_ns,   command.end_ns,   command.record }
            });
        }
        if (hook.kind == Kind::Record || hook.kind == Kind::StreamSynchronize) continue;

        const auto group = events.find({ hook.process, hook.identity });
        const Binding * record = nullptr;
        if (group != events.end()) {
            const auto & values = group->second;
            auto at = std::ranges::upper_bound(values, hook.span.begin_ns, {}, [](const Binding * value) { return value->hook->span.begin_ns; });
            if (at != values.begin()) {
                --at;
                const auto & candidate = **at;
                const bool prior_ordered = at == values.begin()
                                           || ((*std::prev(at))->hook->span.end_ns <= candidate.hook->span.begin_ns
                                               && (*std::prev(at))->hook->span.begin_ns < candidate.hook->span.begin_ns);
                if (candidate.hook->span.begin_ns < hook.span.begin_ns && candidate.hook->span.end_ns <= hook.span.begin_ns && prior_ordered)
                    record = &candidate;
            }
        }
        if (!record || !record->device) {
            ++observations.counts["ascend.event_wait_without_observed_record"];
            continue;
        }
        const auto * consumer = hook.kind == Kind::EventSynchronize ? binding.host : binding.device;
        if (!consumer) {
            ++observations.counts["ascend.event_wait_without_observed_consumer"];
            const auto submitted = binding.host ? binding.host->span.begin_ns : hook.span.begin_ns;
            const auto completed = record->device->span.end_ns;
            const char * reason = "ascend.unrecorded_wait_with_pending_producer";
            if (completed <= submitted) reason = "ascend.unrecorded_wait_with_completed_producer";
            else if (binding.host && completed <= binding.host->span.end_ns) reason = "ascend.unrecorded_wait_completed_during_host_call";
            ++observations.counts[reason];
            continue;
        }

        // The observed wait span already includes blocking. The producer gates
        // its completion, not its start; otherwise source replay charges it twice.
        observations.dependencies.push_back(
            { port(observations, *record->device, true), port(observations, *consumer, true), DependencyKind::Synchronization });
        if (control) observations.device_controls[*control].producer_completed_ns = record->device->span.end_ns;
        ++observations.counts[hook.kind == Kind::EventSynchronize ? "ascend.event_host_synchronizations" : "ascend.event_stream_waits"];
    }
}

Index RuntimeCausality::port(Observations & observations, const Call & call, bool completion) {
    const auto & span = call.span;
    return observations.point({ completion ? span.end_ns : span.begin_ns, observations.canonical_resource(span.resource), span.name, span.record });
}

void RuntimeCausality::connect_streams(Observations & observations, const std::vector<Binding> & bindings) const {
    using RawStream = std::pair<std::string, std::string>;
    std::map<RawStream, std::vector<const Binding *>> commands;
    for (const auto & binding : bindings) {
        if (!binding.host || !binding.device) continue;
        commands[{ binding.hook->process, binding.hook->stream }].push_back(&binding);
    }
    for (auto & [stream, queued] : commands)
        std::ranges::sort(queued, [](const Binding * a, const Binding * b) { return a->hook->span.begin_ns < b->hook->span.begin_ns; });

    for (const auto & binding : bindings) {
        const auto & hook = *binding.hook;
        if (hook.kind != Kind::StreamSynchronize || !binding.host) continue;

        const auto stream = commands.find({ hook.process, hook.stream });
        if (stream == commands.end()) {
            ++observations.counts["ascend.stream_sync_without_recorded_frontier"];
            continue;
        }
        const auto & queued = stream->second;
        auto at = std::ranges::lower_bound(queued, hook.span.begin_ns, {}, [](const Binding * value) { return value->hook->span.begin_ns; });
        if (at == queued.begin()) {
            ++observations.counts["ascend.stream_sync_before_recorded_commands"];
            continue;
        }
        --at;
        const auto & prior = **at;
        const bool ordered = at == queued.begin() || (*std::prev(at))->hook->span.end_ns <= prior.hook->span.begin_ns;
        if (prior.hook->span.end_ns > hook.span.begin_ns || !ordered) {
            ++observations.counts["ascend.stream_sync_with_concurrent_submission"];
            continue;
        }

        // Raw handles prove the queue, unlike a shared physical device lane.
        // Event commands bound a known prefix; unobserved later launches remain
        // outside this partial frontier until capture supplies their queue IDs.
        observations.dependencies.push_back(
            { port(observations, *prior.device, true), port(observations, *binding.host, true), DependencyKind::Synchronization });
        ++observations.counts["ascend.stream_sync_with_recorded_partial_frontier"];
    }
}

void RuntimeCausality::connect(Observations & observations) {
    const auto bindings = bind_calls(observations);
    connect_streams(observations, bindings);
    connect_events(observations, bindings);
}

} // namespace markov::adapters::ascend
