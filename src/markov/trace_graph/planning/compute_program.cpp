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

bool ComputeProgram::append_compute(const Frame & frame, source::Index source_call, std::size_t & at, Segment & segment) {
    const auto & observed = source_.observations();
    const auto unchanged = frame.projection.operations[at].source_work_unchanged;
    const auto & call = observed.work_observations[source_call];
    const auto & description = observed.work_descriptions[call.description];
    if (!description.calls) {
        issue_ = "compute_call_rule_unavailable";
        return false;
    }
    // A source view has no task. Its target copy uses the same proven frame
    // execution context; an observed device call still needs its own binding.
    const auto & binding = source_.compute_resources(source_call);
    if ((!description.observed.empty() && !binding) || (binding && *binding != frame.resources)) {
        issue_ = "compute_call_queue_binding_unavailable";
        return false;
    }
    std::vector<Work> work;
    while (at < frame.projection.operations.size() && frame.projection.operations[at].source_operation == source_call) {
        const auto & instance = frame.projection.operations[at++];
        if (instance.work) work.push_back(frame.projection.work[*instance.work]);
    }
    auto calls = description.calls->describe(frame.phase, frame.input, work);
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
    if (!segment.plan.extend(calls, frame.resources, preparations_)) {
        issue_ = "kernel_cache_initial_state_unavailable";
        return false;
    }
    ++segment.stats.compute_calls;
    segment.stats.compute_services += work.size();
    return true;
}

ComputeProgram::ComputeProgram(const source::SourceContext & source, std::vector<FrameCompute> frames, std::size_t first_resource,
                               PreparationCache & preparations, ExecutionQueues & queues, ComputeScope scope, simulation::Component owner,
                               RequestControls * controls, std::size_t request)
    : source_(source),
      preparations_(preparations),
      queues_(queues),
      owner_(owner),
      controls_(controls),
      request_(request),
      first_resource_(first_resource) {
    const auto & observed = source.observations();
    std::map<std::tuple<source::Index, WorkKind, source::Index>, std::size_t> jobs;
    std::map<source::Index, std::size_t> tails;
    for (auto & input : frames) {
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
        frame.input = input.variables;
        frame.phase = observed.compute_frames[input.frame].phase;
        frame.resources = *resources;
        const auto * result = source.frame_result(input.frame);
        // shortcut: targets inherit source scheduling; require a target policy before supporting mode changes.
        frame.overlap = result && result->overlap;
        frame.publishes_prefill_prefix = frame.phase == Phase::Prefill && result && result->processed && result->processed->finished_after.size() == 1
                                         && !result->processed->finished_after.front();
        stats_.frames.push_back({ input.frame });
        if (!source.frame_cpu_order_supported(input.frame)) {
            issue_ = "overlapping_or_cross_thread_cpu_regions_require_explicit_ports";
            return;
        }
        for (const auto & entry : source.frame_calls(input.frame)) {
            if (entry.kind != source::FrameCallKind::Collective) continue;
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
            frame.segments.push_back({ {}, job->second, static_cast<std::size_t>(member), {} });
        }
        frame.segments.push_back({});
        frame.callers_modeled = scope == ComputeScope::Request && !source.result_sequences(input.frame).empty();
        if (frame.callers_modeled) {
            std::optional<CallResources> result_resources;
            for (const auto sequence : source.result_sequences(input.frame))
                if (const auto placement = source.sequence(sequence).resources.unique_resources()) {
                    result_resources = placement;
                    break;
                }
            for (const auto sequence : source.result_sequences(input.frame)) {
                const auto & site = source.sequence(sequence);
                const auto & call = site.observation;
                if (!call.definition || site.resources.caller != resources->caller) {
                    issue_ = "result_caller_recipe_or_resource_unavailable";
                    return;
                }
                if (call.control && !controls_) {
                    issue_ = "result_control_requires_cache_state";
                    return;
                }
                auto placement = site.resources.unique_resources();
                if (!placement) {
                    if (!site.resources.workers.empty() || !site.resources.devices.empty()) {
                        issue_ = "result_control_queues_unavailable:" + call.role + ":workers=" + std::to_string(site.resources.workers.size())
                                 + ":devices=" + std::to_string(site.resources.devices.size());
                        return;
                    }
                    placement = result_resources.value_or(*resources);
                }
                frame.results.push_back(result_steps_.size());
                result_steps_.push_back({ id, sequence, *placement, {} });
            }
        }
        frame.projection = std::move(input.plan);
    }
    for (const auto & collective : collectives_) {
        if (std::ranges::any_of(collective.peers, [](const auto & peer) { return !peer; })) {
            issue_ = "missing_collective_participant";
            return;
        }
    }
    if (frames_.empty()) issue_ = "empty_compute_group";
}

void ComputeProgram::compile_frame(std::size_t id) {
    const auto & source = source_;
    auto & frame = frames_[id];
    const auto source_frame = stats_.frames[id].source_frame;
    std::size_t first = 0, range = 0;
    for (const auto & entry : source.frame_calls(source_frame)) {
        auto & pending = frame.segments[range];
        if (entry.gap_before) {
            pending.plan.source_cpu_gap(*entry.gap_before, frame.resources.caller);
            ++pending.stats.source_cpu_gaps;
        }
        if (entry.kind == source::FrameCallKind::Compute) {
            if (!append_compute(frame, entry.index, first, pending)) return;
            continue;
        }
        if (entry.kind == source::FrameCallKind::Preparation) {
            const auto & site = source.sequence(entry.index);
            const auto & preparation = site.observation;
            const auto & binding = site.resources;
            if (!preparation.definition || binding.caller != frame.resources.caller || binding.workers.size() != 1
                || *binding.workers.begin() != frame.resources.worker) {
                issue_ = "preparation_rule_or_queues_unavailable";
                return;
            }
            auto calls = preparation.definition->describe(frame.phase, frame.input);
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
                    || std::ranges::any_of(binding.devices, [&](std::size_t device) { return device != *incoming && device != frame.resources.device; })) {
                    issue_ = "preparation_stream_binding_unavailable";
                    return;
                }
                placement = frame.resources;
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
        ++range;
    }
    if (frame.callers_modeled && !frame.segments.back().plan.publishes_result()) {
        issue_ = "result_output_producer_unavailable";
        return;
    }
    frame.projection = {};
}

void ComputeProgram::start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause, std::optional<simulation::Component> previous_owner) {
    if (issue_) return;
    for (const auto root : roots_)
        if (previous_owner)
            queues_.callers.handoff(runtime, frames_[root].resources.caller, *previous_owner, owner_, action(ActionKind::ForwardGranted, root), *cause);
        else launch_frame(runtime, root, cause);
}

void ComputeProgram::launch_frame(ProgramRuntime & runtime, std::size_t id, std::optional<graph::NodeId> cause) {
    queues_.callers.acquire(runtime, frames_[id].resources.caller, owner_, action(ActionKind::ForwardGranted, id), cause);
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
    queues_.callers.acquire(runtime, frame.resources.caller, owner_, action(ActionKind::CallersGranted, id), cause);
}

void ComputeProgram::run_result(ProgramRuntime & runtime, std::size_t id, graph::NodeId cause) {
    auto & frame = frames_[id];
    const auto step_id = frame.results.at(frame.result_at);
    auto & step = result_steps_[step_id];
    const auto & site = source_.sequence(step.sequence);
    const auto & call = site.observation;
    const auto work = call.control ? controls_->begin_result_control(request_, stats_.frames[id].source_frame, *call.control) : std::vector<Work>{};
    auto calls = call.definition->describe(frame.phase, frame.input, work);
    if (calls.issue) {
        issue_ = calls.issue;
        return;
    }
    source_.bind_native_sources(step.sequence, calls.calls);
    if (!step.plan.extend(calls, step.resources, preparations_)) {
        issue_ = "kernel_cache_initial_state_unavailable";
        return;
    }
    const auto begin = runtime.next_node();
    NativeEvents events{ action(ActionKind::ResultStepDone, step_id), action(ActionKind::ResultStepSubmitted, step_id) };
    if (step.plan.publishes_result()) events.produced = action(ActionKind::Sent, id);
    auto mutation = step.plan.mutation(owner_, begin, events, frame.result_at == 0 ? frame.result_producer : std::optional(cause), queues_.native);
    mutation.dependencies.push_back({ cause, begin, graph::EdgeKind::ResourceOrder });
    if (!runtime.submit(std::move(mutation))) return;
    if (frame.result_at == 0) stats_.frames[id].callers_entry = begin;
    for (const auto & [resource, node] : step.plan.frontier(begin)) queues_.native[resource] = node;
    auto & counts = stats_.preparations[call.role];
    ++counts.sequences;
    counts.operations += step.plan.operations();
    stats_.native_operations += step.plan.operations();
}

void ComputeProgram::result_step_submitted(ProgramRuntime & runtime, const simulation::Event & event) {
    const auto id = result_steps_[event.action.subject].frame;
    auto & frame = frames_[id];
    if (++frame.result_at < frame.results.size()) {
        run_result(runtime, id, event.identity);
        return;
    }
    stats_.frames[id].callers_completed = event.identity;
    if (frame.overlap && frame.next) {
        start_next(runtime, *frame.next);
        process_result(runtime, *frame.next);
    }
    else start_next(runtime, id);
    finish_frame(runtime, id);
    join_prefill_results(runtime);
    queues_.callers.release(runtime, frame.resources.caller, owner_, event.identity);
}

void ComputeProgram::result_step_done(ProgramRuntime & runtime, const simulation::Event & event) {
    const auto & step = result_steps_[event.action.subject];
    const auto & call = source_.sequence(step.sequence).observation;
    if (call.control) controls_->end_result_control(runtime, request_, stats_.frames[step.frame].source_frame, *call.control, event.identity);
    auto & frame = frames_[step.frame];
    frame.result_completions.push_back(event.identity);
    if (frame.result_completions.size() != frame.results.size()) return;
    const auto end = runtime.next_node();
    PlannedMutation mutation{ owner_, { { 0, Boundary{}, action(ActionKind::ResultWorkDone, step.frame) } }, {} };
    for (const auto node : frame.result_completions) mutation.dependencies.push_back({ node, end, graph::EdgeKind::DataDependency });
    runtime.submit(std::move(mutation));
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
    if (event.action.kind == std::to_underlying(ActionKind::ForwardGranted)) {
        const auto id = event.action.subject;
        auto & frame = frames_[id];
        if (controls_) frame.input = controls_->begin_frame(request_, stats_.frames[id].source_frame, frame.input);
        stats_.frames[id].allocator_view_offset_pages = frame.input.device_pages_allocated_since_return;
        compile_frame(id);
        if (!issue_) segment(runtime, id, event.identity);
    }
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
        queues_.callers.release(runtime, frames_[id].resources.caller, owner_, event.identity);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::ResultReady)) {
        stats_.frames[event.action.subject].result_ready = event.identity;
        process_result(runtime, event.action.subject);
        join_prefill_results(runtime);
        join_results(runtime);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::ResultStepSubmitted)) result_step_submitted(runtime, event);
    else if (event.action.kind == std::to_underlying(ActionKind::ResultStepDone)) result_step_done(runtime, event);
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
