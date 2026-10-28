#include "markov/trace_graph/planning/compute_program.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <tuple>
#include <utility>

namespace markov::trace_graph::planning {

void ComputeStats::add(const ComputeStats & other) {
    collective_services += other.collective_services;
    participant_arrivals += other.participant_arrivals;
    queue_dependencies += other.queue_dependencies;
    participant_wait_ns += other.participant_wait_ns;
    maximum_arrival_span_ns = std::max(maximum_arrival_span_ns, other.maximum_arrival_span_ns);
}

namespace {

PlannedMutation compute_segment(const source::SourceContext & source, const FrameCompute & frame, std::size_t first, std::size_t last, graph::NodeId begin,
                                simulation::Action completion, std::optional<graph::NodeId> cause) {
    PlannedMutation mutation{ ComputeProgram::owner, {}, {} };
    const auto & plan = frame.plan;
    for (auto at = first; at < last; ++at) {
        const auto & operation = plan.operations[at];
        const auto resource = operation.work ? source.device_service(operation.source_operation).resource.value() : 0;
        if (operation.work) mutation.operations.push_back({ resource, std::cref(plan.work[*operation.work]) });
        else mutation.operations.push_back({ resource, Boundary{} });
    }

    std::vector<bool> has_input(last - first, false), has_successor(last - first, false);
    for (const auto edges : { std::span(plan.dependencies), std::span(plan.queue_dependencies) }) {
        auto at = std::ranges::lower_bound(edges, first, {}, &graph::Edge::from);
        for (; at != edges.end() && at->from < last; ++at) {
            if (at->to < first || at->to >= last) continue;
            mutation.dependencies.push_back({ begin + at->from - first, begin + at->to - first, at->kind });
            has_input[at->to - first] = true;
            has_successor[at->from - first] = true;
        }
    }
    const auto end = begin + mutation.operations.size();
    mutation.operations.push_back({ 0, Boundary{}, completion });
    for (std::size_t id = 0; id < has_successor.size(); ++id) {
        if (cause && !has_input[id]) mutation.dependencies.push_back({ *cause, begin + id, graph::EdgeKind::DataDependency });
        if (!has_successor[id]) mutation.dependencies.push_back({ begin + id, end, graph::EdgeKind::DataDependency });
    }
    if (first == last && cause) mutation.dependencies.push_back({ *cause, end, graph::EdgeKind::DataDependency });
    return mutation;
}

} // namespace

ComputeProgram::ComputeProgram(const source::SourceContext & source, std::vector<FrameCompute> frames, std::size_t first_resource)
    : source_(source),
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
        if (std::ranges::any_of(operations, [&](source::Index operation) { return observed.work_observations[operation].resource != participant; })) {
            issue_ = "cross_thread_compute_requires_explicit_ports";
            return;
        }
        const auto id = frames_.size();
        const auto [tail, inserted] = tails.try_emplace(participant, id);
        if (inserted) roots_.push_back(id);
        else {
            frames_[tail->second].next = id;
            tail->second = id;
        }
        frames_.push_back({ std::move(input), {}, 0, std::nullopt });
        auto & frame = frames_.back();
        std::size_t first = 0;
        for (const auto call_id : source.frame_collectives(frame.compute.frame)) {
            const auto & call = observed.collectives[call_id];
            const auto & group = observed.collective_groups[call.group];
            if (call.resource != participant) {
                issue_ = "collective_and_compute_have_different_callers";
                return;
            }
            auto last = first;
            while (last < frame.compute.plan.operations.size()
                   && observed.work_observations[frame.compute.plan.operations[last].source_operation].end_ns <= call.begin_ns)
                ++last;
            if (last < frame.compute.plan.operations.size()
                && observed.work_observations[frame.compute.plan.operations[last].source_operation].begin_ns < call.end_ns) {
                issue_ = "overlapping_compute_collective_requires_explicit_ports";
                return;
            }

            auto work = call.work;
            const auto items = call.rows.basis == RowBasis::QueryTokens ? frame.compute.variables.query_tokens : frame.compute.variables.batch_size;
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
            frame.segments.push_back({ first, last, job->second, static_cast<std::size_t>(member) });
            first = last;
        }
        frame.segments.push_back({ first, frame.compute.plan.operations.size(), std::nullopt, 0 });
        std::vector<std::size_t> segment_ids(frame.compute.plan.operations.size());
        for (std::size_t segment = 0; segment < frame.segments.size(); ++segment) {
            const auto & range = frame.segments[segment];
            std::fill(segment_ids.begin() + range.first, segment_ids.begin() + range.last, segment);
        }
        for (const auto & edge : frame.compute.plan.queue_dependencies) {
            if (segment_ids[edge.from] > segment_ids[edge.to]) {
                issue_ = "source_queue_order_contradicts_collective_fence";
                return;
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

void ComputeProgram::start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause) {
    if (issue_) return;
    for (const auto root : roots_) segment(runtime, root, cause);
}

void ComputeProgram::segment(ProgramRuntime & runtime, std::size_t id, std::optional<graph::NodeId> cause) {
    const auto & frame = frames_[id];
    const auto & range = frame.segments[frame.at];
    auto mutation = compute_segment(source_, frame.compute, range.first, range.last, runtime.next_node(), action(ActionKind::SegmentDone, id), cause);
    const auto queues = std::ranges::count(mutation.dependencies, graph::EdgeKind::ResourceOrder, &graph::Edge::kind);
    if (runtime.submit(std::move(mutation))) stats_.queue_dependencies += queues;
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
