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
    participant_wait_ns += other.participant_wait_ns;
    maximum_arrival_span_ns = std::max(maximum_arrival_span_ns, other.maximum_arrival_span_ns);
}

bool ComputeProgram::append_compute(const FrameCompute & frame, std::size_t first, std::size_t last, CallResources resources, Segment & segment) {
    const auto & observed = source_.observations();
    for (auto at = first; at < last;) {
        const auto source_call = frame.plan.operations[at].source_operation;
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
        while (at < last && frame.plan.operations[at].source_operation == source_call) {
            const auto & instance = frame.plan.operations[at++];
            if (instance.work) work.push_back(frame.plan.work[*instance.work]);
        }
        auto calls = description.calls->describe(observed.compute_frames[frame.frame].phase, frame.variables, work);

        // A single unchanged device operation has a source service candidate.
        // The cost provider checks its exclusive ownership before migration.
        if (unchanged && work.size() == 1) {
            for (auto & native : calls)
                if (native.launches == work) native.source_operation = source_call;
        }
        segment.plan.extend(calls, resources);
        ++segment.stats.compute_calls;
        segment.stats.compute_services += work.size();
    }
    return true;
}

ComputeProgram::ComputeProgram(const source::SourceContext & source, std::vector<FrameCompute> frames, std::size_t first_resource)
    : source_(source),
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
        if (std::ranges::any_of(operations, [&](source::Index operation) { return observed.work_observations[operation].resource != participant; })) {
            issue_ = "cross_thread_compute_requires_explicit_ports";
            return;
        }
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
        struct Fence {
            source::Time begin, end;
            source::Index id;
            bool preparation;
        };
        std::vector<Fence> fences;
        for (const auto call_id : source.frame_collectives(input.frame)) {
            const auto & call = observed.collectives[call_id];
            fences.push_back({ call.begin_ns, call.end_ns, call_id, false });
        }
        const auto & preparations = observed.compute_frames[input.frame].preparations;
        const auto preparation_resources = source.preparation_resources(input.frame);
        for (std::size_t i = 0; i < preparations.size(); ++i) {
            if (!preparations[i].definition || !preparation_resources[i] || preparation_resources[i]->caller != resources->caller
                || preparation_resources[i]->worker != resources->worker) {
                issue_ = "preparation_rule_or_resources_unavailable";
                return;
            }
            fences.push_back({ preparations[i].region.begin_ns, preparations[i].region.end_ns, i, true });
        }
        std::ranges::sort(fences, {}, &Fence::begin);
        std::size_t first = 0;
        Segment pending;
        for (const auto & fence : fences) {
            auto last = first;
            while (last < input.plan.operations.size() && observed.work_observations[input.plan.operations[last].source_operation].end_ns <= fence.begin)
                ++last;
            if (last < input.plan.operations.size() && observed.work_observations[input.plan.operations[last].source_operation].begin_ns < fence.end) {
                issue_ = "overlapping_compute_call_requires_explicit_ports";
                return;
            }
            if (!append_compute(input, first, last, *resources, pending)) return;
            first = last;
            if (fence.preparation) {
                const auto & preparation = preparations[fence.id];
                const auto calls = preparation.definition->describe(observed.compute_frames[input.frame].phase, input.variables);
                const auto before = pending.plan.operations();
                // Preparation may produce inputs on another device stream.
                // NativePlan retains that producer before subsequent forward work.
                pending.plan.extend(calls, *preparation_resources[fence.id]);
                auto & stats = pending.stats.preparations[preparation.role];
                stats.operations += pending.plan.operations() - before;
                ++stats.sequences;
                continue;
            }
            const auto & call = observed.collectives[fence.id];
            const auto & group = observed.collective_groups[call.group];
            if (call.resource != participant) {
                issue_ = "collective_and_compute_have_different_callers";
                return;
            }
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
        if (!append_compute(input, first, input.plan.operations.size(), *resources, pending)) return;
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
