#include "markov/trace_graph/planning/cache_program.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace markov::trace_graph::planning {

bool CacheProgram::eviction_backup(ProgramRuntime & runtime, TransferId transfer, RadixId node, std::size_t pages) {
    const Features features{
        {           "pages",                                   pages },
        {      "page_bytes", page_tokens_ * backend_.bytes_per_token },
        {          "layers",                         backend_.layers },
        { "bytes_per_token",                backend_.bytes_per_token }
    };
    const std::vector<Work> work{
        { backend_.backup_caller, features }
    };
    const FrameInput input{ 0, 0, 0, 1, static_cast<double>(page_tokens_) };
    std::vector<NativePlan> plans(backend_.participants);
    for (std::size_t rank = 0; rank < plans.size(); ++rank) {
        auto calls = backend_.backup_calls->describe(Phase::Prefill, input, work);
        if (calls.issue) {
            issue_ = calls.issue;
            return false;
        }
        const auto resource = source_.observations().resources.size() + 2 * rank + 1;
        for (auto & call : calls.calls)
            if (call.async_work)
                call.async_completion = AsyncCompletion{
                    { backend_.backup_completion, features },
                    resource,
                    std::nullopt,
                    0
                };
        const auto caller = running_[request_].callers[rank];

        // This closed host recipe has no separate worker or compute stream.
        if (!plans[rank].extend(calls, { caller, caller, caller }, preparations_)) {
            issue_ = "kernel_cache_initial_state_unavailable";
            return false;
        }
    }

    const auto begin = runtime.next_node();
    PlannedMutation mutation{ 0, {}, {} };
    PendingCopy pending{ node, true, CopyKind::Backup, pages, std::nullopt, cache_.tree().nodes()[node].pages };
    NativeFrontier frontier;
    for (std::size_t rank = 0; rank < plans.size(); ++rank) {
        const auto base = begin + mutation.operations.size();
        const auto resource = source_.observations().resources.size() + 2 * rank + 1;
        const auto ends = plans[rank].frontier(base);
        auto participant = plans[rank].mutation(0, base, { std::nullopt }, cause_, queues_.native);
        pending.caller_returns.push_back(base + participant.operations.size() - 1);
        pending.copy_completions.push_back(ends.at(resource));
        frontier.insert(ends.begin(), ends.end());
        mutation.operations.insert(mutation.operations.end(), participant.operations.begin(), participant.operations.end());
        mutation.dependencies.insert(mutation.dependencies.end(), participant.dependencies.begin(), participant.dependencies.end());
    }
    const auto end = begin + mutation.operations.size();
    mutation.operations.push_back({ 0, Boundary{}, action(ActionKind::CopyDone, transfer) });
    for (const auto port : pending.caller_returns) mutation.dependencies.push_back({ port, end, graph::EdgeKind::DataDependency });
    for (const auto port : pending.copy_completions) mutation.dependencies.push_back({ port, end, graph::EdgeKind::DataDependency });
    pending.completion = end;
    copies_.emplace(transfer, std::move(pending));
    if (!runtime.submit(std::move(mutation))) return false;
    for (const auto & [resource, tail] : frontier) queues_.native[resource] = tail;
    copy_frontier_[1] = end;
    ++backups_;
    return true;
}

FrameInput CacheProgram::begin_frame(std::size_t request, std::size_t frame, FrameInput input) {
    auto & running = running_[request];
    const auto context = static_cast<std::size_t>(input.context_tokens);
    auto & views = running.allocation_views;
    auto at = views.find(context);
    if (at == views.end()) {
        const auto previous = views.empty() ? requests_[request].device_hit_pages * page_tokens_ : views.rbegin()->first;
        const auto pages = (context + page_tokens_ - 1) / page_tokens_ - (previous + page_tokens_ - 1) / page_tokens_;
        const auto offset = cache_.device_pages_allocated_since_return();
        cache_.allocate_reserved(*running.buffer, pages);
        at = views.emplace(context, offset).first;
    }
    input.device_pages_allocated_since_return = at->second;
    if (source_.observations().compute_frames[frame].phase == Phase::Prefill) requests_[request].device_pages_allocated_since_return = at->second;
    return input;
}

std::size_t CacheProgram::release_group(std::size_t request, std::size_t frame, ResultControl group) {
    // TP participants share one logical iteration, not one source frame ID.
    const auto iteration = static_cast<std::size_t>(source_.observations().compute_frames[frame].sequence_tokens.front());
    auto & groups = running_[request].release_groups;
    const auto [at, inserted] = groups.try_emplace(std::pair(iteration, group), release_groups_.size());
    if (inserted) release_groups_.push_back({ request, {}, std::nullopt, {}, std::nullopt });
    return at->second;
}

std::optional<ControlWork> CacheProgram::begin_result_control(std::size_t request, std::size_t frame, ResultControlPoint point) {
    const auto operation = point.operation;
    if (operation != ResultControl::BackupUpdatedPrefix && operation != ResultControl::BackupCommittedPrefix)
        return ControlWork{ result_control_work(request, frame, point) };

    auto & running = running_[request];
    const bool prefix = operation == ResultControl::BackupUpdatedPrefix;
    const auto & insertion = prefix ? running.prefix_insertion : running.final_insertion;
    if (!insertion) return std::nullopt;
    auto at = running.backups.find(operation);
    if (at == running.backups.end()) {
        std::vector<TransferId> transfers;
        std::vector<RadixId> path;
        for (auto node = insertion->leaf; node != 0; node = cache_.tree().nodes()[node].parent) path.push_back(node);
        const bool needed = std::ranges::any_of(path, [&](RadixId node) { return cache_.backup_required(node); });
        if (needed && (!insertion->completed || (!prefix && running.final_publication))) return std::nullopt;
        for (auto node = path.rbegin(); node != path.rend(); ++node) {
            if (!cache_.backup_required(*node) || !make_host_room(cache_.tree().nodes()[*node].pages.size())) continue;
            const auto started = cache_.begin_backup(*node);
            if (started.status != CopyStatus::Started) continue;
            transfers.push_back(*started.transfer);
            copies_.emplace(*started.transfer, PendingCopy{ *node, false, CopyKind::Backup, started.pages, std::nullopt, cache_.tree().nodes()[*node].pages });
            ++backups_;
        }
        at = running.backups.emplace(operation, std::move(transfers)).first;
    }
    const auto caller = source_.observations().work_observations[source_.frame_operations(frame).front()].resource;
    const auto rank = std::ranges::find(running.callers, caller) - running.callers.begin();
    const auto resource = source_.observations().resources.size() + 2 * rank + 1;
    ControlWork work;
    for (const auto transfer : at->second) {
        const auto & copy = copies_.at(transfer);
        const double page_bytes = page_tokens_ * backend_.bytes_per_token;
        const Features features{
            {           "pages",               copy.pages },
            {      "page_bytes",               page_bytes },
            {          "layers",          backend_.layers },
            { "bytes_per_token", backend_.bytes_per_token }
        };
        work.completions.push_back({
            { backend_.backup_completion, features },
            resource,
            action(ActionKind::CopyParticipantDone, transfer),
            0
        });
        work.work.push_back({ backend_.backup_caller, features });
    }
    return work;
}

void CacheProgram::join_copy(ProgramRuntime & runtime, TransferId transfer) {
    auto & copy = copies_.at(transfer);
    if (copy.completion || copy.copy_completions.size() != backend_.participants || copy.caller_returns.size() != backend_.participants) return;
    const auto end = runtime.next_node();
    PlannedMutation mutation{ 0, { { 0, Boundary{}, action(ActionKind::CopyDone, transfer) } }, {} };
    for (const auto node : copy.copy_completions) mutation.dependencies.push_back({ node, end, graph::EdgeKind::DataDependency });
    for (const auto node : copy.caller_returns) mutation.dependencies.push_back({ node, end, graph::EdgeKind::DataDependency });
    if (runtime.submit(std::move(mutation))) {
        copy.completion = end;
        copy_frontier_[1] = end;
    }
}

std::vector<Work> CacheProgram::result_control_work(std::size_t request, std::size_t frame, ResultControlPoint point) {
    const auto control = point.operation;
    auto & running = running_[request];
    switch (control) {
    case ResultControl::UpdateDuplicates:
    case ResultControl::CommitDuplicates:
    case ResultControl::ReleaseTail: {
        const auto & observed = source_.observations();
        const auto committed = observed.token_paths.length(observed.request_tokens.at(observed.requests[request].identity).committed.value().path);
        const auto tokens = control == ResultControl::ReleaseTail        ? committed % page_tokens_
                            : control == ResultControl::UpdateDuplicates ? running.prefix_insertion->duplicate_pages * page_tokens_
                                                                         : running.final_insertion->duplicate_pages * page_tokens_;
        auto & group = release_groups_[release_group(request, frame, point.release_group)];
        group.indices.try_emplace(control, tokens);
        return {
            { { "allocator.release_indices", {}, {} }, { { "released_tokens", tokens } } }
        };
    }
    case ResultControl::ReleaseGroup:
    case ResultControl::ConvertReleasedIndices:
    case ResultControl::PublishReleasedPages: {
        auto & group = release_groups_[release_group(request, frame, point.release_group)];
        if (!group.free_pages) group.free_pages = cache_.free_device_pages();
        std::size_t tokens = 0, pages = 0, tensors = 0;
        for (const auto & [point, count] : group.indices) {
            tokens += count;
            pages += (count + page_tokens_ - 1) / page_tokens_;
            if (count) ++tensors;
        }
        return {
            { { "allocator.release_group", {}, {} },
             { { "released_tokens", tokens }, { "released_pages", pages }, { "input_tensors", tensors }, { "free_pages", *group.free_pages } } }
        };
    }
    default:
        break;
    }
    if (control == ResultControl::MatchPrefix) {
        if (running.prefix_match) return *running.prefix_match;
        const auto leaf = running.prefix_insertion->leaf;
        std::size_t tensors = 0;
        for (auto node = leaf; node != 0; node = cache_.tree().nodes()[node].parent)
            if (cache_.node(node).device) ++tensors;
        const auto pages = cache_.page_count(leaf);
        running.prefix_match = std::vector<Work>{
            {         { "cache.index_match", {}, {} },         { { "pages", pages }, { "input_tensors", tensors } } },
            { { "cache.request_table_write", {}, {} }, { { "pages", pages - requests_[request].device_hit_pages } } }
        };
        return *running.prefix_match;
    }
    auto & insertion = control == ResultControl::UpdatePrefix ? running.prefix_insertion : running.final_insertion;
    if (insertion) return insertion->work;
    const auto & observed = source_.observations();
    const auto & tokens = observed.request_tokens.at(observed.requests[request].identity);
    const auto path = control == ResultControl::UpdatePrefix ? tokens.prompt.value().path : tokens.committed.value().path;
    const auto pages = observed.token_paths.pages(path, page_tokens_);
    const auto publication = cache_.begin_device_publication();
    auto committed = cache_.commit_device(pages, *running.buffer, publication);
    const auto protected_pages = control == ResultControl::UpdatePrefix || !running.prefix_insertion ? requests_[request].device_hit_pages
                                                                                                     : cache_.page_count(running.prefix_insertion->leaf);
    insertion = RunningRequest::Insertion{ committed.node, pages.size() - protected_pages - committed.allocated_pages, {}, {}, std::nullopt };
    if (control == ResultControl::UpdatePrefix) running.prefix_publication = publication;
    for (const auto & copy : committed.index_copies)
        insertion->work.push_back({
            { "cache.index_clone", {}, {} },
            { { "pages", copy.pages }, { "host", copy.tier == Tier::Host ? 1 : 0 } }
        });
    cache_.unpin_device(*running.pin);
    running.pin = committed.node;
    cache_.pin_device(*running.pin);
    if (control == ResultControl::CommitPrefix) {
        running.committed = committed.node;
        running.final_publication = publication;
    }
    return insertion->work;
}

void CacheProgram::end_result_control(ProgramRuntime & runtime, std::size_t request, std::size_t frame, ResultControlPoint point, graph::NodeId completion) {
    const auto control = point.operation;
    if (control == ResultControl::BackupUpdatedPrefix || control == ResultControl::BackupCommittedPrefix) {
        for (const auto transfer : running_[request].backups.at(control)) {
            copies_.at(transfer).caller_returns.push_back(completion);
            join_copy(runtime, transfer);
        }
        return;
    }
    if (control == ResultControl::PublishReleasedPages) {
        const auto group_id = release_group(request, frame, point.release_group);
        auto & group = release_groups_[group_id];
        group.completions.push_back(completion);
        if (group.completions.size() != backend_.participants) return;
        const auto end = runtime.next_node();
        PlannedMutation mutation{ 0, { { 0, Boundary{}, action(ActionKind::ReleasedPages, group_id) } }, {} };
        for (const auto node : group.completions) mutation.dependencies.push_back({ node, end, graph::EdgeKind::DataDependency });
        runtime.submit(std::move(mutation));
        return;
    }
    if (control != ResultControl::UpdatePrefix && control != ResultControl::CommitPrefix) return;
    auto & running = running_[request];
    auto & insertion = control == ResultControl::UpdatePrefix ? running.prefix_insertion : running.final_insertion;
    insertion->completions.push_back(completion);
    if (insertion->completions.size() != backend_.participants) return;
    const auto end = runtime.next_node();
    PlannedMutation mutation{
        0,
        { { 0, Boundary{}, action(control == ResultControl::UpdatePrefix ? ActionKind::PrefixInserted : ActionKind::FinalInserted, request) } },
        {}
    };
    for (const auto node : insertion->completions) mutation.dependencies.push_back({ node, end, graph::EdgeKind::DataDependency });
    runtime.submit(std::move(mutation));
}

void CacheProgram::complete_insertion(ProgramRuntime & runtime, std::size_t request, ResultControl control) {
    auto & running = running_[request];
    auto & insertion = control == ResultControl::UpdatePrefix ? running.prefix_insertion : running.final_insertion;
    insertion->completed = cause_;
    if (control == ResultControl::CommitPrefix) {
        if (running.outputs_ready && running.final_publication) publish_result(runtime, request);
        return;
    }
    cache_.publish_all(*running.prefix_publication);
    cache_.acknowledge(*running.prefix_publication);
    running.prefix_publication.reset();
    requests_[request].prefill_prefix_published = cause_;
    running.compute->resume_controls(runtime, *cause_);
    if (running.compute->issue()) issue_ = running.compute->issue();
    if (waiting_) advance(runtime);
}

} // namespace markov::trace_graph::planning
