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
    if (!segment.plan.extend(calls.calls, resources, preparations_)) {
        issue_ = "kernel_cache_initial_state_unavailable";
        return false;
    }
    ++segment.stats.compute_calls;
    segment.stats.compute_services += work.size();
    return true;
}

ComputeProgram::ComputeProgram(const source::SourceContext & source, std::vector<FrameCompute> frames, std::size_t first_resource,
                               PreparationCache & preparations)
    : source_(source),
      preparations_(preparations),
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
        if (inserted) roots_.push_back(id);
        else {
            frames_[tail->second].next = id;
            tail->second = id;
        }
        frames_.push_back({});
        auto & frame = frames_.back();
        if (!source.frame_cpu_order_supported(input.frame)) {
            issue_ = "overlapping_or_cross_thread_cpu_regions_require_explicit_ports";
            return;
        }
        const auto & preparations = observed.compute_frames[input.frame].preparations;
        const auto preparation_bindings = source.preparation_bindings(input.frame);
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
                const auto & preparation = preparations[entry.index];
                const auto & binding = preparation_bindings[entry.index];
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
                    if (const auto & issue = source.preparation_native_calls(input.frame, entry.index).issue) {
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
                source.bind_native_cpu_sources(input.frame, entry.index, calls.calls);
                const auto before = pending.plan.operations();
                // Preparation may produce inputs on another device stream.
                // NativePlan retains that producer before subsequent forward work.
                if (!pending.plan.extend(calls.calls, *placement, preparations_)) {
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
    }
    for (const auto & collective : collectives_) {
        if (std::ranges::any_of(collective.peers, [](const auto & peer) { return !peer; })) {
            issue_ = "missing_collective_participant";
            return;
        }
    }
    if (frames_.empty()) issue_ = "empty_compute_group";
}

void ComputeProgram::start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause) {
    if (issue_) return;
    for (const auto root : roots_) segment(runtime, root, cause);
}

void ComputeProgram::segment(ProgramRuntime & runtime, std::size_t id, std::optional<graph::NodeId> cause) {
    const auto & frame = frames_[id];
    const auto & range = frame.segments[frame.at];
    if (runtime.submit(range.plan.mutation(owner, runtime.next_node(), action(ActionKind::SegmentDone, id), cause))) {
        stats_.add(range.stats);
        stats_.native_operations += range.plan.operations();
        stats_.dispatch_delays += range.plan.delays();
    }
}

void ComputeProgram::arrive(ProgramRuntime & runtime, std::size_t id, const simulation::Event & event) {
    auto & frame = frames_[id];
    const auto & range = frame.segments[frame.at];
    if (!range.collective) {
        if (frame.next) segment(runtime, *frame.next, event.identity);
        else {
            completed_.push_back(event.identity);
            if (completed_.size() == roots_.size()) {
                const auto end = runtime.next_node();
                PlannedMutation mutation{ owner, { { 0, Boundary{}, action(ActionKind::Complete, 0) } }, {} };
                for (const auto node : completed_) mutation.dependencies.push_back({ node, end, graph::EdgeKind::DataDependency });
                runtime.submit(std::move(mutation));
            }
        }
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
    PlannedMutation mutation{ owner, { { first_resource_ + collective.group, std::cref(work), action(ActionKind::CollectiveDone, *range.collective) } }, {} };
    for (const auto & arrival : collective.arrivals) mutation.dependencies.push_back({ arrival->node, begin, graph::EdgeKind::Synchronization });
    if (runtime.submit(std::move(mutation))) {
        ++stats_.collective_services;
        stats_.maximum_arrival_span_ns = std::max(stats_.maximum_arrival_span_ns, last - first);
        for (const auto & arrival : collective.arrivals) stats_.participant_wait_ns += last - arrival->time_ns;
    }
}

void ComputeProgram::on_event(ProgramRuntime & runtime, const simulation::Event & event) {
    if (event.action.kind == std::to_underlying(ActionKind::SegmentDone)) arrive(runtime, event.action.subject, event);
    else if (event.action.kind == std::to_underlying(ActionKind::CollectiveDone)) {
        for (const auto peer : collectives_[event.action.subject].peers) {
            ++frames_[*peer].at;
            segment(runtime, *peer, event.identity);
        }
    }
    else complete_ = true;
}

} // namespace markov::trace_graph::planning
