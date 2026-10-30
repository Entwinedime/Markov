#include "markov/trace_graph/planning/region_compute.hpp"
#include "markov/trace_graph/source/regions.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace markov::trace_graph::planning {

void RegionCompute::identify(ProgramRuntime & runtime, PlannedMutation & mutation, const Frame & frame, std::string_view port) const {
    if (!runtime.records_graph()) return;

    const auto participant = source_.observations().compute_frames[frame.description.source_frame].participant;
    for (auto & operation : mutation.operations) operation.position = OperationPosition{ request_, frame.group, participant, port };
    if (std::holds_alternative<SourceRegionGap>(mutation.operations.front().cost))
        mutation.operations.front().position->port = port == "prepare" ? "before_prepare" : port == "batch" ? "before_batch" : "before_result";
}

Features RegionPreparation::enter(std::size_t caller, Phase phase, const FrameInput & input, std::span<const Work> preparations) {
    const auto first = phases_.emplace(caller, phase).second;
    std::size_t fresh = 0;
    for (const auto & preparation : preparations) fresh += preparations_.emplace(caller, preparation).second;
    const bool empty = initial_ == KernelCacheInitial::Empty;
    return {
        {     "query_tokens",                                                           input.query_tokens },
        {   "context_tokens",                                                         input.context_tokens },
        {  "attention_pairs", input.query_tokens * (2 * input.context_tokens - input.query_tokens + 1) / 2 },
        {       "batch_size",                                                             input.batch_size },
        {      "page_tokens",                                                            input.page_tokens },
        {      "first_phase",                                                       empty && first ? 1 : 0 },
        { "new_preparations",                                                  empty && !first ? fresh : 0 }
    };
}

RegionCompute::RegionCompute(const source::SourceContext & source, std::vector<RegionFrame> frames, RegionBackend backend, std::size_t first_resource,
                             RegionPreparation & preparation, ExecutionQueues & queues, RequestControls & controls, std::size_t request)
    : source_(source),
      backend_(std::move(backend)),
      first_resource_(first_resource),
      request_(request),
      owner_(first_owner + request),
      preparation_(preparation),
      queues_(queues),
      controls_(controls) {
    if (!preparation.initial()) {
        issue_ = "region_initialization_state_unavailable";
        return;
    }
    std::set<std::size_t> callers;
    for (const auto & frame : frames) {
        const auto * result = source.frame_result(frame.source_frame);
        const auto & observed = source.observations().compute_frames[frame.source_frame];
        if (!result || !observed.preparation_region || observed.requests.size() != 1 || !source.regions().frame(frame.source_frame).before_preparation) {
            issue_ = "semantic_frame_boundaries_unavailable";
            return;
        }
        callers.insert(result->run.resource);
    }
    std::map<std::size_t, std::size_t> previous;
    std::map<std::pair<Phase, double>, std::size_t> groups;
    for (auto & description : frames) {
        const auto id = frames_.size();
        const auto & result = *source.frame_result(description.source_frame);
        const auto phase = source.observations().compute_frames[description.source_frame].phase;
        const auto rank = std::distance(callers.begin(), callers.find(result.run.resource));
        auto [group, inserted] = groups.try_emplace({ phase, description.input.context_tokens }, groups_.size());
        if (inserted) groups_.push_back({});
        groups_[group->second].frames.push_back(id);
        const auto prior = previous.find(result.run.resource);
        std::optional<std::size_t> before = prior == previous.end() ? std::nullopt : std::optional(prior->second);
        Frame frame{};
        frame.description = description;
        frame.caller = result.run.resource;
        frame.rank = rank;
        frame.group = group->second;
        frame.phase = phase;
        frame.previous = before;
        frame.overlap = result.overlap;
        frame.has_result = result.processed.has_value();
        frame.publishes = phase == Phase::Prefill && result.processed && !result.processed->finished_after.front();
        frame.sends = !result.final_sends.empty();
        if (before) frames_[*before].next = id;
        else roots_.push_back(id);
        previous[result.run.resource] = id;
        frames_.push_back(std::move(frame));
        stats_.frames.emplace_back();
        stats_.frames.back().source_frame = description.source_frame;
    }
    for (const auto & group : groups_) {
        if (group.frames.size() != callers.size()) {
            issue_ = "semantic_frame_participants_incomplete";
            return;
        }
    }
}

void RegionCompute::start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause, std::optional<simulation::Component> previous_owner) {
    if (issue_) return;
    for (const auto root : roots_) {
        const auto caller = frames_[root].caller;
        if (previous_owner) queues_.callers.handoff(runtime, caller, *previous_owner, owner_, action(ActionKind::Granted, root), *cause);
        else queues_.callers.acquire(runtime, caller, owner_, action(ActionKind::Granted, root), cause);
    }
}

void RegionCompute::prepare(ProgramRuntime & runtime, std::size_t id, graph::NodeId cause) {
    auto & frame = frames_[id];
    frame.description.input = controls_.begin_frame(request_, frame.description.source_frame, frame.description.input);
    const auto & observed = source_.observations().compute_frames[frame.description.source_frame];
    const auto & region = *observed.preparation_region;
    std::vector<Work> preparations;
    for (const auto & sequence : observed.preparations) {
        if (sequence.region.resource != region.resource || sequence.region.begin_ns < region.begin_ns || sequence.region.end_ns > region.end_ns) continue;
        if (!sequence.definition) {
            issue_ = "semantic_preparation_rule_unavailable";
            return;
        }
        const auto calls = sequence.definition->describe(frame.phase, frame.description.input, sequence.source_work);
        if (calls.issue) {
            issue_ = calls.issue;
            return;
        }
        for (const auto & call : calls.calls)
            if (call.preparation) preparations.push_back(call.preparation->specialization);
    }
    frame.features = preparation_.enter(frame.caller, frame.phase, frame.description.input, preparations);
    const auto phase = frame.phase == Phase::Prefill ? "prefill" : "decode";
    const auto work = [&](std::string role, std::string scope) {
        return Work{
            { role + "." + phase, backend_.implementation, scope },
            frame.features
        };
    };
    frame.prepare_cpu = work("prepare", "thread_cpu");
    frame.prepare_stream = work("prepare", "current_stream_interval");
    frame.prepare_return = work("prepare", "caller_blocking_interval");
    frame.batch_cpu = work("batch", "thread_cpu");
    frame.batch_stream = work("batch", "current_stream_interval");
    frame.batch_return = work("batch", "caller_blocking_interval");
    frame.result_cpu = work("result", "thread_cpu");
    frame.result_cpu.kind.implementation = backend_.result_implementation;
    const auto & result = source_.frame_result(frame.description.source_frame)->processed;
    if (result) frame.result_cpu.features["finished"] = result->finished_after.front() ? 1 : 0;
    if (frame.sends) {
        frame.result_cpu.kind.operation = std::string("result_send.") + phase;
        frame.result_tail = frame.result_cpu;
        frame.result_tail.kind.operation = std::string("result_tail.") + phase;
    }

    const auto begin = runtime.next_node();
    const auto gap = *source_.regions().frame(frame.description.source_frame).before_preparation;
    PlannedMutation mutation{
        owner_,
        { { frame.caller, Boundary{} },
          { frame.caller, std::cref(frame.prepare_cpu) },
          { first_resource_ + 2 * frame.rank, std::cref(frame.prepare_stream) },
          { frame.caller, std::cref(frame.prepare_return), std::nullopt, true },
          { frame.caller, Boundary{}, action(ActionKind::Prepared, id) } },
        { { cause, begin, graph::EdgeKind::ResourceOrder },
          { begin, begin + 1, graph::EdgeKind::DataDependency },
          { begin, begin + 2, graph::EdgeKind::DataDependency },
          { begin, begin + 3, graph::EdgeKind::DataDependency },
          { begin + 1, begin + 4, graph::EdgeKind::DataDependency },
          { begin + 3, begin + 4, graph::EdgeKind::DataDependency } }
    };

    // Request admission owns the root control interval. Later frames retain
    // their own pre-preparation residual, without recharging that first gap.
    if (frame.previous) mutation.operations.front().cost = SourceRegionGap{ gap };
    identify(runtime, mutation, frame, "prepare");

    if (runtime.submit(std::move(mutation))) {
        stats_.frames[id].entry = begin;
        frame.input_ready = begin + 2;
        if (frame.previous) ++stats_.source_cpu_gaps;
        ++stats_.preparations["semantic_input"].sequences;
        stats_.preparations["semantic_input"].operations += 2;
    }
}

void RegionCompute::run_frame(ProgramRuntime & runtime, std::size_t id, graph::NodeId cause) {
    auto & frame = frames_[id];
    const auto gap = source_.regions().frame(frame.description.source_frame).before_run;
    if (!gap) {
        issue_ = "semantic_forward_gap_unavailable";
        return;
    }
    const auto begin = runtime.next_node();
    PlannedMutation mutation{
        owner_,
        { { frame.caller, SourceRegionGap{ *gap } },
          { frame.caller, Boundary{}, action(ActionKind::Entered, id) },
          { frame.caller, std::cref(frame.batch_cpu) },
          { frame.caller, std::cref(frame.batch_return), std::nullopt, true },
          { frame.caller, Boundary{}, action(ActionKind::Submitted, id) } },
        { { cause, begin, graph::EdgeKind::ResourceOrder },
          { begin, begin + 1, graph::EdgeKind::DataDependency },
          { begin + 1, begin + 2, graph::EdgeKind::DataDependency },
          { begin + 1, begin + 3, graph::EdgeKind::DataDependency },
          { begin + 2, begin + 4, graph::EdgeKind::DataDependency },
          { begin + 3, begin + 4, graph::EdgeKind::DataDependency } }
    };
    identify(runtime, mutation, frame, "batch");
    runtime.submit(std::move(mutation));
}

void RegionCompute::launch_group(ProgramRuntime & runtime, std::size_t group_id) {
    auto & group = groups_[group_id];
    if (group.launched || std::ranges::any_of(group.frames, [&](std::size_t id) { return !frames_[id].entered; })) return;
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ owner_, {}, {} };
    for (const auto id : group.frames) {
        const auto & frame = frames_[id];
        const auto node = begin + mutation.operations.size();
        mutation.operations.push_back({ first_resource_ + 2 * frame.rank + 1, std::cref(frame.batch_stream), action(ActionKind::Ready, id) });
        if (runtime.records_graph())
            mutation.operations.back().position =
                OperationPosition{ request_, frame.group, source_.observations().compute_frames[frame.description.source_frame].participant, "batch" };
        for (const auto peer : group.frames) {
            mutation.dependencies.push_back({ *frames_[peer].entered, node, graph::EdgeKind::Synchronization });
            mutation.dependencies.push_back({ *frames_[peer].input_ready, node, graph::EdgeKind::DataDependency });
        }
        if (frame.previous) mutation.dependencies.push_back({ *stats_.frames[*frame.previous].completed, node, graph::EdgeKind::DataDependency });
    }
    group.launched = runtime.submit(std::move(mutation)).has_value();
    if (group.launched) {
        for (std::size_t at = 0; at < group.frames.size(); ++at) stats_.frames[group.frames[at]].completed = begin + at;
        stats_.compute_calls += group.frames.size();
        stats_.compute_services += group.frames.size();
    }
}

void RegionCompute::advance(ProgramRuntime & runtime, std::size_t id) {
    auto & frame = frames_[id];
    const auto & ports = stats_.frames[id];
    if (frame.next && !frame.next_started && ports.submitted
        && (frame.overlap ? !frame.previous || !frames_[*frame.previous].has_result || stats_.frames[*frame.previous].callers_completed
                          : ports.result_ready && (!frame.has_result || ports.callers_completed))) {
        frame.next_started = true;
        queues_.callers.acquire(runtime, frame.caller, owner_, action(ActionKind::Granted, *frame.next), ports.submitted);
    }
    const auto & group = groups_[frame.group];
    const bool ready = std::ranges::all_of(group.frames, [&](std::size_t peer) { return stats_.frames[peer].result_ready.has_value(); });
    if (frame.has_result && !frame.result_requested && ports.submitted && ready
        && (!frame.previous || !frames_[*frame.previous].has_result || stats_.frames[*frame.previous].callers_completed)
        && (!frame.overlap || !frame.next || stats_.frames[*frame.next].submitted)) {
        frame.result_requested = true;
        const auto cause = frame.overlap && frame.next ? stats_.frames[*frame.next].submitted : ports.submitted;
        queues_.callers.acquire(runtime, frame.caller, owner_, action(ActionKind::ResultGranted, id), cause);
    }
}

void RegionCompute::process_result(ProgramRuntime & runtime, std::size_t id, graph::NodeId cause) {
    const auto & frame = frames_[id];
    const auto gap = source_.regions().frame(frame.description.source_frame).before_result;
    if (!gap) {
        issue_ = "semantic_result_gap_unavailable";
        return;
    }
    const auto begin = runtime.next_node();
    PlannedMutation mutation{
        owner_,
        {        { frame.caller, SourceRegionGap{ *gap } },
          { frame.caller, std::cref(frame.result_cpu), action(frame.sends ? ActionKind::SentFrame : ActionKind::ResultDone, id) } },
        { { cause, begin, graph::EdgeKind::ResourceOrder },                { begin, begin + 1, graph::EdgeKind::DataDependency } }
    };
    for (const auto peer : groups_[frame.group].frames)
        mutation.dependencies.push_back({ *stats_.frames[peer].result_ready, begin, graph::EdgeKind::DataDependency });
    if (frame.sends) {
        mutation.operations.push_back({ frame.caller, std::cref(frame.result_tail), action(ActionKind::ResultDone, id) });
        mutation.dependencies.push_back({ begin + 1, begin + 2, graph::EdgeKind::DataDependency });
    }
    identify(runtime, mutation, frame, "result");
    runtime.submit(std::move(mutation));
}

graph::NodeId RegionCompute::join(ProgramRuntime & runtime, std::span<const graph::NodeId> causes, ActionKind kind) {
    const auto node = runtime.next_node();
    PlannedMutation mutation{ owner_, { { 0, Boundary{}, action(kind, 0) } }, {} };
    for (const auto cause : causes) mutation.dependencies.push_back({ cause, node, graph::EdgeKind::DataDependency });
    runtime.submit(std::move(mutation));
    return node;
}

void RegionCompute::finish(ProgramRuntime & runtime) {
    std::vector<graph::NodeId> prefill, results, sends, finished;
    bool prefill_done = true, results_done = true, sends_done = true, all_done = true;
    for (std::size_t id = 0; id < frames_.size(); ++id) {
        auto & frame = frames_[id];
        const auto & ports = stats_.frames[id];
        if (frame.publishes) {
            prefill_done &= ports.callers_completed.has_value();
            if (ports.callers_completed) prefill.push_back(*ports.callers_completed);
        }
        if (frame.sends) {
            sends_done &= ports.sent.has_value();
            if (ports.sent) sends.push_back(*ports.sent);
        }
        results_done &= ports.result_ready.has_value();
        if (ports.result_ready) results.push_back(*ports.result_ready);
        const bool finished_frame = ports.submitted && ports.result_ready && (!frame.has_result || ports.callers_completed);
        all_done &= finished_frame;
        if (finished_frame) {
            finished.push_back(*ports.submitted);
            finished.push_back(*ports.result_ready);
            if (ports.callers_completed) finished.push_back(*ports.callers_completed);
        }
    }
    if (!prefill_joined_ && prefill_done && !prefill.empty()) {
        prefill_joined_ = true;
        join(runtime, prefill, ActionKind::PrefillReady);
    }
    if (!results_joined_ && results_done) {
        results_joined_ = true;
        join(runtime, results, ActionKind::ResultsReady);
    }
    if (!sent_joined_ && sends_done && !sends.empty()) {
        sent_joined_ = true;
        join(runtime, sends, ActionKind::Sent);
    }
    if (!done_joined_ && all_done) {
        done_joined_ = true;
        join(runtime, finished, ActionKind::Done);
    }
}

void RegionCompute::on_event(ProgramRuntime & runtime, const simulation::Event & event) {
    const auto kind = static_cast<ActionKind>(event.action.kind);
    const auto id = event.action.subject;
    if (kind == ActionKind::Granted) prepare(runtime, id, event.identity);
    else if (kind == ActionKind::Prepared) run_frame(runtime, id, event.identity);
    else if (kind == ActionKind::Entered) {
        frames_[id].entered = event.identity;
        launch_group(runtime, frames_[id].group);
    }
    else if (kind == ActionKind::Submitted) {
        stats_.frames[id].submitted = event.identity;
        queues_.callers.release(runtime, frames_[id].caller, owner_, event.identity);
        advance(runtime, id);
        if (frames_[id].previous) advance(runtime, *frames_[id].previous);
    }
    else if (kind == ActionKind::Ready) {
        stats_.frames[id].result_ready = event.identity;
        for (const auto peer : groups_[frames_[id].group].frames) advance(runtime, peer);
    }
    else if (kind == ActionKind::ResultGranted) process_result(runtime, id, event.identity);
    else if (kind == ActionKind::SentFrame) stats_.frames[id].sent = event.identity;
    else if (kind == ActionKind::ResultDone) {
        stats_.frames[id].callers_completed = event.identity;
        stats_.frames[id].result_work_completed = event.identity;
        queues_.callers.release(runtime, frames_[id].caller, owner_, event.identity);
        advance(runtime, id);
        if (frames_[id].next) advance(runtime, *frames_[id].next);
    }
    else if (kind == ActionKind::PrefillReady) prefill_result_ready_ = event.identity;
    else if (kind == ActionKind::ResultsReady) results_ready_ = event.identity;
    else if (kind == ActionKind::Sent) sent_ = event.identity;
    else if (kind == ActionKind::Done) complete_ = true;
    if (!issue_ && !complete_) finish(runtime);
}

} // namespace markov::trace_graph::planning
