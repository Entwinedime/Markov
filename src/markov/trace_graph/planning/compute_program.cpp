#include "markov/trace_graph/planning/compute_program.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace markov::trace_graph::planning {

void ComputeStats::add(const ComputeStats & other) {
    collective_services += other.collective_services;
    participant_arrivals += other.participant_arrivals;
    compute_calls += other.compute_calls;
    compute_services += other.compute_services;
    for (const auto & [role, values] : other.preparations) {
        preparations[role].sequences += values.sequences;
        preparations[role].operations += values.operations;
    }
    native_operations += other.native_operations;
    dispatch_delays += other.dispatch_delays;
    source_cpu_gaps += other.source_cpu_gaps;
    frames.insert(frames.end(), other.frames.begin(), other.frames.end());
    participant_wait_ns += other.participant_wait_ns;
    maximum_arrival_span_ns = std::max(maximum_arrival_span_ns, other.maximum_arrival_span_ns);
}

bool ComputeProgram::append_compute(const FrameCompute & frame, source::Index source_call, std::size_t & at, CallResources resources, Segment & segment) {
    const auto & observed = source_.observations();
    const auto unchanged = frame.plan.operations[at].source_work_unchanged;
    const auto & call = observed.work_observations[source_call];
    const auto & description = observed.work_descriptions[call.description];
    if (!description.calls) {
        issue_ = "compute_call_rule_unavailable";
        return false;
    }
    // A source view has no task. Its target copy uses the same proven frame
    // execution context; an observed device call still needs its own binding.
    const auto & binding = source_.compute_resources(source_call);
    if ((!description.observed.empty() && !binding) || (binding && *binding != resources)) {
        issue_ = "compute_call_queue_binding_unavailable";
        return false;
    }
    std::vector<Work> work;
    while (at < frame.plan.operations.size() && frame.plan.operations[at].source_operation == source_call) {
        const auto & instance = frame.plan.operations[at++];
        if (instance.work) work.push_back(frame.plan.work[*instance.work]);
    }
    auto calls = description.calls->describe(observed.compute_frames[frame.frame].phase, frame.variables, work);
    if (calls.issue) {
        issue_ = calls.issue;
        return false;
    }

    // A single unchanged device operation has a source service candidate.
    // The cost provider checks its exclusive ownership before migration.
    if (unchanged && work.size() == 1) {
        for (auto & native : calls.calls)
            if (native.launches == work) native.source_operation = source_call;
    }
    if (!segment.plan.extend(calls, resources, preparations_)) {
        issue_ = "kernel_cache_initial_state_unavailable";
        return false;
    }
    ++segment.stats.compute_calls;
    segment.stats.compute_services += work.size();
    return true;
}

ComputeProgram::ComputeProgram(const source::SourceContext & source, std::vector<FrameCompute> frames, std::size_t first_resource,
                               PreparationCache & preparations, ExecutionQueues & queues, simulation::Component owner)
    : source_(source),
      preparations_(preparations),
      queues_(queues),
      owner_(owner),
      first_resource_(first_resource) {
    const auto & observed = source.observations();
    std::map<std::tuple<source::Index, WorkKind, source::Index>, std::size_t> jobs;
    std::map<source::Index, std::size_t> tails;
    for (const auto & input : frames) {
        const auto operations = source.frame_operations(input.frame);
        if (operations.empty() || !input.plan.missing_row_bindings.empty() || !source.frame_queue_issues(input.frame).empty()) {
            issue_ = "unresolved_compute_work_or_queue";
            return;
        }
        const auto participant = observed.work_observations[operations.front()].resource;
        if (std::ranges::any_of(input.plan.queue_dependencies, [](const graph::Edge & edge) { return edge.from > edge.to; })) {
            issue_ = "source_stream_order_contradicts_caller_order";
            return;
        }
        std::optional<CallResources> resources;
        for (const auto call : operations)
            if (source.compute_resources(call)) {
                resources = source.compute_resources(call);
                break;
            }
        if (!resources) {
            issue_ = "frame_execution_resources_unavailable";
            return;
        }
        const auto id = frames_.size();
        const auto [tail, inserted] = tails.try_emplace(participant, id);
        std::optional<std::size_t> previous;
        if (inserted) roots_.push_back(id);
        else {
            previous = tail->second;
            frames_[*previous].next = id;
            tail->second = id;
        }
        frames_.push_back({});
        auto & frame = frames_.back();
        frame.previous = previous;
        frame.caller = resources->caller;
        const auto * result = source.frame_result(input.frame);
        // shortcut: targets inherit source scheduling; require a target policy before supporting mode changes.
        frame.overlap = result && result->overlap;
        frame.publishes_prefill_prefix = observed.compute_frames[input.frame].phase == Phase::Prefill && result && result->processed
                                         && result->processed->finished_after.size() == 1 && !result->processed->finished_after.front();
        stats_.frames.push_back({ input.frame, {}, {}, {}, {}, {}, {}, {}, {} });
        if (!source.frame_cpu_order_supported(input.frame)) {
            issue_ = "overlapping_or_cross_thread_cpu_regions_require_explicit_ports";
            return;
        }
        std::size_t first = 0;
        Segment pending;
        for (const auto & entry : source.frame_calls(input.frame)) {
            if (entry.gap_before) {
                pending.plan.source_cpu_gap(*entry.gap_before, participant);
                ++pending.stats.source_cpu_gaps;
            }
            if (entry.kind == source::FrameCallKind::Compute) {
                if (!append_compute(input, entry.index, first, *resources, pending)) return;
                continue;
            }
            if (entry.kind == source::FrameCallKind::Preparation) {
                const auto & site = source.sequence(entry.index);
                const auto & preparation = site.observation;
                const auto & binding = site.resources;
                if (!preparation.definition || binding.caller != resources->caller || binding.workers.size() != 1
                    || *binding.workers.begin() != resources->worker) {
                    issue_ = "preparation_rule_or_queues_unavailable";
                    return;
                }
                auto calls = preparation.definition->describe(observed.compute_frames[input.frame].phase, input.variables);
                if (calls.issue) {
                    issue_ = calls.issue;
                    return;
                }
                if (!calls.calls.empty() && std::ranges::all_of(calls.calls, [](const NativeCall & call) { return !call.queue_name.empty(); })) {
                    if (const auto & issue = site.native.issue) {
                        issue_ = issue;
                        return;
                    }
                }
                auto placement = binding.unique_resources();
                if (std::ranges::any_of(calls.calls, [](const NativeCall & call) { return call.incoming_stream; })) {
                    const auto incoming = pending.plan.producer_device();
                    if (!incoming
                        || std::ranges::any_of(binding.devices, [&](std::size_t device) { return device != *incoming && device != resources->device; })) {
                        issue_ = "preparation_stream_binding_unavailable";
                        return;
                    }
                    placement = resources;
                }
                if (!placement) {
                    issue_ = "preparation_device_binding_unavailable";
                    return;
                }
                source.bind_native_sources(entry.index, calls.calls);
                const auto before = pending.plan.operations();
                // Preparation may produce inputs on another device stream.
                // NativePlan retains that producer before subsequent forward work.
                if (!pending.plan.extend(calls, *placement, preparations_)) {
                    issue_ = "kernel_cache_initial_state_unavailable";
                    return;
                }
                auto & stats = pending.stats.preparations[preparation.role];
                stats.operations += pending.plan.operations() - before;
                ++stats.sequences;
                continue;
            }
            const auto & call = observed.collectives[entry.index];
            const auto & group = observed.collective_groups[call.group];
            auto work = call.work;
            const auto items = call.rows.basis == RowBasis::QueryTokens ? input.variables.query_tokens : input.variables.batch_size;
            work.features["rows"] = items * call.rows.rows_per_item;
            work.features["logical_tensor_bytes"] = work.features.at("rows") * work.features.at("width") * work.features.at("element_bytes");
            const auto [job, created] = jobs.try_emplace(std::tuple(call.group, work.kind, call.sequence), collectives_.size());
            if (created)
                collectives_.push_back({ work,
                                         call.group,
                                         std::vector<std::optional<std::size_t>>(group.members.size()),
                                         std::vector<std::optional<Arrival>>(group.members.size()) });
            auto & collective = collectives_[job->second];
            const auto member = std::ranges::find(group.members, call.rank) - group.members.begin();
            if (collective.work != work || collective.peers[member]) {
                issue_ = "collective_payload_or_membership_mismatch";
                return;
            }
            collective.peers[member] = id;
            pending.collective = job->second;
            pending.participant = static_cast<std::size_t>(member);
            frame.segments.push_back(std::move(pending));
            pending = Segment{};
        }
        frame.segments.push_back(std::move(pending));
        frame.callers_modeled = frame.segments.back().plan.publishes_result() && !source.result_sequences(input.frame).empty();
        if (frame.callers_modeled) {
            for (const auto sequence : source.result_sequences(input.frame)) {
                const auto & site = source.sequence(sequence);
                const auto & call = site.observation;
                if (!call.definition || site.resources.caller != resources->caller) {
                    issue_ = "result_caller_recipe_or_resource_unavailable";
                    return;
                }
                auto calls = call.definition->describe(observed.compute_frames[input.frame].phase, input.variables);
                if (calls.issue) {
                    issue_ = calls.issue;
                    return;
                }
                auto placement = site.resources.unique_resources();
                if (!placement) {
                    if (!site.resources.workers.empty() || !site.resources.devices.empty()
                        || std::ranges::any_of(calls.calls, [](const NativeCall & value) { return value.kind != CallKind::Cpu; })) {
                        issue_ = "result_control_queues_unavailable";
                        return;
                    }
                    placement = resources;
                }
                source.bind_native_sources(sequence, calls.calls);
                const auto before = frame.result_callers.operations();
                if (!frame.result_callers.extend(calls, *placement, preparations_)) {
                    issue_ = "kernel_cache_initial_state_unavailable";
                    return;
                }
                auto & stats = frame.caller_stats.preparations[call.role];
                ++stats.sequences;
                stats.operations += frame.result_callers.operations() - before;
            }
        }
    }
    for (const auto & collective : collectives_) {
        if (std::ranges::any_of(collective.peers, [](const auto & peer) { return !peer; })) {
            issue_ = "missing_collective_participant";
            return;
        }
    }
    if (frames_.empty()) issue_ = "empty_compute_group";
}

void ComputeProgram::start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause, std::optional<simulation::Component> previous_owner) {
    if (issue_) return;
    for (const auto root : roots_)
        if (previous_owner) queues_.callers.handoff(runtime, frames_[root].caller, *previous_owner, owner_, action(ActionKind::ForwardGranted, root), *cause);
        else launch_frame(runtime, root, cause);
}

void ComputeProgram::launch_frame(ProgramRuntime & runtime, std::size_t id, std::optional<graph::NodeId> cause) {
    queues_.callers.acquire(runtime, frames_[id].caller, owner_, action(ActionKind::ForwardGranted, id), cause);
}

void ComputeProgram::segment(ProgramRuntime & runtime, std::size_t id, std::optional<graph::NodeId> cause) {
    auto & frame = frames_[id];
    const auto & range = frame.segments[frame.at];
    const auto begin = runtime.next_node();
    NativeEvents events{ action(ActionKind::SegmentDone, id) };
    if (!range.collective) events.submitted = action(ActionKind::FrameSubmitted, id);
    if (range.plan.publishes_result()) events.produced = action(ActionKind::ResultReady, id);
    if (runtime.submit(range.plan.mutation(owner_, begin, events, cause, queues_.native))) {
        if (frame.at == 0) stats_.frames[id].entry = begin;
        if (range.plan.publishes_result()) frame.result_producer = range.plan.result_producer(begin);
        for (const auto & [resource, node] : range.plan.frontier(begin)) queues_.native[resource] = node;
        stats_.add(range.stats);
        stats_.native_operations += range.plan.operations();
        stats_.dispatch_delays += range.plan.delays();
    }
}

void ComputeProgram::arrive(ProgramRuntime & runtime, std::size_t id, const simulation::Event & event) {
    auto & frame = frames_[id];
    const auto & range = frame.segments[frame.at];
    if (!range.collective) {
        stats_.frames[id].completed = event.identity;
        if (!frame.overlap) start_next(runtime, id);
        finish_frame(runtime, id);
        return;
    }

    auto & collective = collectives_[*range.collective];
    collective.arrivals[range.participant] = Arrival{ runtime.now_ns(), event.identity };
    ++stats_.participant_arrivals;
    if (std::ranges::any_of(collective.arrivals, [](const auto & arrival) { return !arrival; })) return;

    auto work = collective.work;
    graph::Time first = std::numeric_limits<graph::Time>::max(), last = 0;
    for (const auto & arrival : collective.arrivals) {
        first = std::min(first, arrival->time_ns);
        last = std::max(last, arrival->time_ns);
    }
    // Independent observations measure completion after the last ready input.
    // The arrival coordinate uses the same integer-us grid as those samples.
    work.features["arrival_span_us"] = static_cast<double>(last / 1'000 - first / 1'000);

    // shortcut: peers share the latest completion; split when per-rank completion evidence is available.
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ owner_, { { first_resource_ + collective.group, std::cref(work), action(ActionKind::CollectiveDone, *range.collective) } }, {} };
    for (const auto & arrival : collective.arrivals) mutation.dependencies.push_back({ arrival->node, begin, graph::EdgeKind::Synchronization });
    if (runtime.submit(std::move(mutation))) {
        ++stats_.collective_services;
        stats_.maximum_arrival_span_ns = std::max(stats_.maximum_arrival_span_ns, last - first);
        for (const auto & arrival : collective.arrivals) stats_.participant_wait_ns += last - arrival->time_ns;
    }
}

void ComputeProgram::start_next(ProgramRuntime & runtime, std::size_t id) {
    auto & frame = frames_[id];
    if (!frame.next || frame.next_started) return;
    const auto & ports = stats_.frames[id];
    if (frame.overlap) {
        if (!ports.submitted) return;
        if (frame.previous && frames_[*frame.previous].callers_modeled && !stats_.frames[*frame.previous].callers_completed) return;
    }
    else if (!ports.completed || (frame.callers_modeled && !ports.callers_completed)) return;
    frame.next_started = true;
    launch_frame(runtime, *frame.next, frame.overlap ? ports.submitted : ports.callers_completed ? ports.callers_completed : ports.completed);
}

void ComputeProgram::process_result(ProgramRuntime & runtime, std::size_t id) {
    auto & frame = frames_[id];
    const auto & ports = stats_.frames[id];
    if (!frame.callers_modeled || frame.callers_requested || !frame.result_producer || !ports.submitted) return;
    if (frame.previous && frames_[*frame.previous].callers_modeled && !stats_.frames[*frame.previous].callers_completed) return;
    if (frame.overlap && frame.next && !stats_.frames[*frame.next].submitted) return;

    frame.callers_requested = true;
    const auto cause = frame.overlap && frame.next ? stats_.frames[*frame.next].submitted : ports.submitted;
    queues_.callers.acquire(runtime, frame.caller, owner_, action(ActionKind::CallersGranted, id), cause);
}

void ComputeProgram::run_result(ProgramRuntime & runtime, std::size_t id, graph::NodeId granted) {
    auto & frame = frames_[id];
    const auto & ports = stats_.frames[id];
    const auto begin = runtime.next_node();
    NativeEvents events{ action(ActionKind::ResultWorkDone, id), action(ActionKind::CallersDone, id) };
    if (frame.result_callers.publishes_result()) events.produced = action(ActionKind::Sent, id);
    auto mutation = frame.result_callers.mutation(owner_, begin, events, frame.result_producer, queues_.native);
    mutation.dependencies.push_back({ granted, begin, graph::EdgeKind::ResourceOrder });
    mutation.dependencies.push_back({ *ports.submitted, begin, graph::EdgeKind::ResourceOrder });
    if (frame.overlap && frame.next) mutation.dependencies.push_back({ *stats_.frames[*frame.next].submitted, begin, graph::EdgeKind::ResourceOrder });
    if (frame.previous && stats_.frames[*frame.previous].callers_completed)
        mutation.dependencies.push_back({ *stats_.frames[*frame.previous].callers_completed, begin, graph::EdgeKind::ResourceOrder });
    if (!runtime.submit(std::move(mutation))) return;
    stats_.frames[id].callers_entry = begin;
    for (const auto & [resource, node] : frame.result_callers.frontier(begin)) queues_.native[resource] = node;
    stats_.add(frame.caller_stats);
    stats_.native_operations += frame.result_callers.operations();
}

void ComputeProgram::finish_frame(ProgramRuntime & runtime, std::size_t id) {
    auto & frame = frames_[id];
    const auto & ports = stats_.frames[id];
    if (frame.finished || !ports.completed || (frame.callers_modeled && (!ports.callers_completed || !ports.result_work_completed))) return;
    frame.finished = true;
    completed_.push_back(*ports.completed);
    if (ports.callers_completed) completed_.push_back(*ports.callers_completed);
    if (ports.result_work_completed) completed_.push_back(*ports.result_work_completed);
    if (++finished_frames_ != frames_.size()) return;

    const auto end = runtime.next_node();
    PlannedMutation mutation{ owner_, { { 0, Boundary{}, action(ActionKind::Complete, 0) } }, {} };
    for (const auto node : completed_) mutation.dependencies.push_back({ node, end, graph::EdgeKind::DataDependency });
    runtime.submit(std::move(mutation));
}

void ComputeProgram::join_prefill_results(ProgramRuntime & runtime) {
    if (prefill_join_submitted_) return;

    for (const auto root : roots_) {
        const auto & frame = frames_[root];
        if (!frame.publishes_prefill_prefix || !stats_.frames[root].result_ready) return;
        if (frame.callers_modeled && !stats_.frames[root].callers_completed) return;
        if (frame.overlap && frame.next && !stats_.frames[*frame.next].submitted) return;
    }
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ owner_, { { 0, Boundary{}, action(ActionKind::PrefillReady, 0) } }, {} };
    for (const auto root : roots_) {
        const auto & frame = frames_[root];
        mutation.dependencies.push_back(
            { stats_.frames[root].callers_completed.value_or(*stats_.frames[root].result_ready), begin, graph::EdgeKind::DataDependency });
        if (frame.overlap && frame.next) mutation.dependencies.push_back({ *stats_.frames[*frame.next].submitted, begin, graph::EdgeKind::ResourceOrder });
    }
    prefill_join_submitted_ = runtime.submit(std::move(mutation)).has_value();
}

void ComputeProgram::join_results(ProgramRuntime & runtime) {
    if (results_join_submitted_ || std::ranges::any_of(stats_.frames, [](const ComputeStats::FramePorts & frame) { return !frame.result_ready; })) return;
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ owner_, { { 0, Boundary{}, action(ActionKind::ResultsReady, 0) } }, {} };
    for (const auto & frame : stats_.frames) mutation.dependencies.push_back({ *frame.result_ready, begin, graph::EdgeKind::DataDependency });
    results_join_submitted_ = runtime.submit(std::move(mutation)).has_value();
}

void ComputeProgram::on_event(ProgramRuntime & runtime, const simulation::Event & event) {
    if (event.action.kind == std::to_underlying(ActionKind::ForwardGranted)) segment(runtime, event.action.subject, event.identity);
    else if (event.action.kind == std::to_underlying(ActionKind::CallersGranted)) run_result(runtime, event.action.subject, event.identity);
    else if (event.action.kind == std::to_underlying(ActionKind::SegmentDone)) arrive(runtime, event.action.subject, event);
    else if (event.action.kind == std::to_underlying(ActionKind::CollectiveDone)) {
        for (const auto peer : collectives_[event.action.subject].peers) {
            ++frames_[*peer].at;
            segment(runtime, *peer, event.identity);
        }
    }
    else if (event.action.kind == std::to_underlying(ActionKind::FrameSubmitted)) {
        const auto id = event.action.subject;
        stats_.frames[id].submitted = event.identity;
        if (frames_[id].overlap) start_next(runtime, id);
        if (frames_[id].previous) process_result(runtime, *frames_[id].previous);
        process_result(runtime, id);
        join_prefill_results(runtime);
        queues_.callers.release(runtime, frames_[id].caller, owner_, event.identity);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::ResultReady)) {
        stats_.frames[event.action.subject].result_ready = event.identity;
        process_result(runtime, event.action.subject);
        join_prefill_results(runtime);
        join_results(runtime);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::CallersDone)) {
        const auto id = event.action.subject;
        stats_.frames[id].callers_completed = event.identity;
        if (frames_[id].overlap && frames_[id].next) {
            start_next(runtime, *frames_[id].next);
            process_result(runtime, *frames_[id].next);
        }
        else start_next(runtime, id);
        finish_frame(runtime, id);
        join_prefill_results(runtime);
        queues_.callers.release(runtime, frames_[id].caller, owner_, event.identity);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::Sent)) {
        stats_.frames[event.action.subject].sent = event.identity;
        sent_ = event.identity;
    }
    else if (event.action.kind == std::to_underlying(ActionKind::ResultWorkDone)) {
        stats_.frames[event.action.subject].result_work_completed = event.identity;
        finish_frame(runtime, event.action.subject);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::ResultsReady)) results_ready_ = event.identity;
    else if (event.action.kind == std::to_underlying(ActionKind::PrefillReady)) prefill_result_ready_ = event.identity;
    else if (event.action.kind == std::to_underlying(ActionKind::Complete)) complete_ = true;
}

} // namespace markov::trace_graph::planning
