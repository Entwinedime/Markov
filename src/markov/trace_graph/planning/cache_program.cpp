#include "markov/trace_graph/planning/cache_program.hpp"

#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/planning/compute_program.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <utility>

namespace markov::trace_graph::planning {

CacheProgram::CacheProgram(const source::SourceContext & source, std::size_t page_tokens, CacheConfig config, CacheBackend backend,
                           std::optional<StorageConfig> storage, std::optional<KernelCacheInitial> kernel_cache_initial)
    : source_(source),
      page_tokens_(page_tokens),
      cache_(config),
      backend_(std::move(backend)),
      preparations_(kernel_cache_initial),
      running_(source.observations().requests.size()),
      requests_(source.observations().requests.size()) {
    if (source.graph().requests.size() != requests_.size()) {
        issue_ = "request_boundaries_unavailable";
        return;
    }
    for (std::size_t request = 0; request < requests_.size(); ++request) {
        std::set<std::size_t> callers;
        for (const auto frame : source.request_frames(source.observations().requests[request].identity)) {
            const auto operations = source.frame_operations(frame);
            if (!operations.empty()) callers.insert(source.observations().work_observations[operations.front()].resource);
        }
        if (callers.size() != backend_.participants) {
            issue_ = "request_caller_resources_unavailable";
            return;
        }
        running_[request].callers.assign(callers.begin(), callers.end());
    }
    if (storage)
        storage_.emplace(cache_,
                         source.observations().token_paths,
                         page_tokens_,
                         backend_.bytes_per_token,
                         backend_.participants,
                         source.observations().resources.size() + 2 * backend_.participants,
                         requests_.size(),
                         *storage,
                         backend_.storage);
}

bool CacheProgram::make_host_room(std::size_t pages) {
    host_evictions_ += cache_.reclaim_host(pages);
    return cache_.available_pages(Tier::Host) >= pages;
}

ComputeStats CacheProgram::compute_stats() const {
    auto stats = compute_stats_;
    for (const auto & running : running_)
        if (running.compute) stats.add(running.compute->stats());
    return stats;
}

bool CacheProgram::submit(ProgramRuntime & runtime, PlannedMutation mutation) {
    if (cause_) {
        const auto begin = runtime.next_node();
        std::vector<bool> has_input(mutation.operations.size(), false);
        for (const auto & edge : mutation.dependencies)
            if (edge.from >= begin) has_input[edge.to - begin] = true;
        for (std::size_t id = 0; id < has_input.size(); ++id) {
            if (!has_input[id]) mutation.dependencies.push_back({ *cause_, begin + id, graph::EdgeKind::DataDependency });
        }
    }
    return runtime.submit(std::move(mutation)).has_value();
}

bool CacheProgram::copy(ProgramRuntime & runtime, TransferId transfer, RadixId node, CopyKind kind, std::size_t pages, bool evict_after) {
    const double page_bytes = page_tokens_ * backend_.bytes_per_token;
    const Work work{
        kind == CopyKind::Load ? backend_.load : backend_.backup,
        { { "bytes", pages * page_bytes }, { "operations", 1 }, { "page_bytes", page_bytes } }
    };
    const auto begin = runtime.next_node();
    copies_.emplace(transfer,
                    PendingCopy{ node,
                                 evict_after,
                                 kind,
                                 pages,
                                 begin + backend_.participants,
                                 kind == CopyKind::Backup ? cache_.tree().nodes()[node].pages : std::vector<PrefixId>{} });
    PlannedMutation mutation{ 0, {}, {} };
    for (std::size_t rank = 0; rank < backend_.participants; ++rank) {
        const auto resource = source_.observations().resources.size() + 2 * rank + (kind == CopyKind::Load ? 0 : 1);
        mutation.operations.push_back({ resource, std::cref(work) });
        mutation.dependencies.push_back({ begin + rank, begin + backend_.participants, graph::EdgeKind::DataDependency });
    }
    mutation.operations.push_back({ 0, Boundary{}, action(ActionKind::CopyDone, transfer) });
    const auto submitted = submit(runtime, std::move(mutation));
    if (submitted) {
        kind == CopyKind::Load ? ++loads_ : ++backups_;
        copy_frontier_[kind == CopyKind::Load ? 0 : 1] = begin + backend_.participants;
    }
    return submitted;
}

bool CacheProgram::make_device_room(ProgramRuntime & runtime, std::size_t pages) {
    std::size_t reclaiming = 0;
    std::vector<graph::NodeId> backups;
    for (const auto node : cache_.eviction_order(Tier::Device)) {
        if (cache_.available_pages(Tier::Device) + reclaiming >= pages) break;
        const auto outcome = cache_.evict_device(node);
        if (outcome == Eviction::Removed) ++device_evictions_;
        else if (outcome == Eviction::BackupRequired && make_host_room(cache_.tree().nodes()[node].pages.size())) {
            const auto started = cache_.begin_backup(node);
            if (started.status == CopyStatus::Started) {
                if (!copy(runtime, *started.transfer, node, CopyKind::Backup, started.pages, true)) return false;
                reclaiming += started.pages;
                backups.push_back(copies_.at(*started.transfer).completion);
            }
        }
    }
    if (!backups.empty()) {
        eviction_wait_ = true;
        const auto end = runtime.next_node();
        PlannedMutation batch{ 0, { { 0, Boundary{}, action(ActionKind::EvictionDone, request_) } }, {} };
        for (const auto completion : backups) batch.dependencies.push_back({ completion, end, graph::EdgeKind::DataDependency });
        submit(runtime, std::move(batch));
        return false;
    }
    if (cache_.available_pages(Tier::Device) >= pages) return true;
    const bool pending_compute = std::ranges::any_of(running_, [](const RunningRequest & request) {
        return request.compute || request.final_publication || request.prefix_publication;
    });
    if (copies_.empty() && !pending_compute && (!storage_ || storage_->idle())) issue_ = "device_capacity_unavailable";
    return false;
}

void CacheProgram::finish_compute(ProgramRuntime & runtime, std::size_t request) {
    auto & running = running_[request];
    if (!running.compute || !running.compute->complete()) return;
    for (const auto & [iteration, group] : running.release_groups)
        if (!release_groups_[group].completed) return;
    if (!requests_[request].response) commit(runtime, request);
    if (running.final_publication && (!running.final_insertion || running.final_insertion->completed)) publish_result(runtime, request);

    // A response does not release storage still used by in-flight work.
    cache_.release(*running.buffer);
    running.buffer.reset();
    cache_.unpin_device(*running.pin);
    running.pin.reset();
    requests_[request].work_completed = cause_;
    compute_stats_.add(running.compute->stats());
    running.compute.reset();
}

void CacheProgram::start(ProgramRuntime & runtime) { advance(runtime); }

void CacheProgram::begin_request(ProgramRuntime & runtime, bool drain) {
    const auto & endpoint = source_.graph().requests[request_];
    const auto resource = source_.graph().graph.nodes()[endpoint.ingress].resource;
    const auto begin = runtime.next_node();
    PlannedMutation mutation{
        0,
        { { resource, Boundary{} }, { resource, SourceDelay{ endpoint.ingress }, action(ActionKind::Received, request_) } },
        { { begin, begin + 1, graph::EdgeKind::ClientOrder } }
    };

    // A drain may finish after the previous response; retain both causes.
    if (request_) mutation.dependencies.push_back({ *requests_[request_ - 1].response, begin, graph::EdgeKind::ClientOrder });
    if (drain) {
        for (const auto tail : copy_frontier_)
            if (tail) mutation.dependencies.push_back({ *tail, begin, graph::EdgeKind::Synchronization });
        if (storage_)
            for (const auto tail : storage_->completion_frontier())
                if (tail) mutation.dependencies.push_back({ *tail, begin, graph::EdgeKind::Synchronization });
    }
    if (!submit(runtime, std::move(mutation))) return;
    requests_[request_].start = begin;
    requests_[request_].arrived = begin + 1;
    request_stage_ = RequestStage::Ingress;
    waiting_ = false;
}

void CacheProgram::respond(ProgramRuntime & runtime, std::size_t request) {
    const auto & endpoint = source_.graph().requests[request];
    const auto resource = source_.graph().graph.nodes()[endpoint.completion].resource;
    const auto response = runtime.next_node();
    if (!submit(runtime, { 0, { { resource, SourceDelay{ endpoint.completion }, action(ActionKind::ResponseDone, request) } }, {} })) return;
    requests_[request].response = response;
    request_stage_ = RequestStage::Response;
    waiting_ = false;
}

void CacheProgram::wait_for_progress(ProgramRuntime & runtime) {
    // A blocked admission yields the caller so older results can release space.
    for (const auto caller : running_[request_].callers) queues_.callers.release(runtime, caller, 0, *cause_);
    running_[request_].admission.clear();
    request_stage_ = RequestStage::Waiting;
}

void CacheProgram::advance(ProgramRuntime & runtime) {
    if (issue_ || eviction_wait_ || request_ == requests_.size()) return;
    waiting_ = true;
    if (request_stage_ == RequestStage::BeforeStart) {
        const auto & barriers = source_.observations().workload_barriers;
        const auto drain = std::ranges::any_of(barriers, [&](const source::WorkloadBarrier & barrier) { return barrier.before_request == request_; });
        const bool pending_compute = std::ranges::any_of(running_, [](const RunningRequest & request) {
            return request.compute || request.final_publication || request.prefix_publication;
        });
        if (drain && (pending_compute || !copies_.empty() || (storage_ && !storage_->idle()))) return;
        begin_request(runtime, drain);
        return;
    }
    if (request_stage_ == RequestStage::Waiting) {
        request_stage_ = RequestStage::Admitting;
        for (const auto caller : running_[request_].callers)
            queues_.callers.acquire(runtime, caller, 0, action(ActionKind::AdmissionGranted, request_), cause_);
        return;
    }
    if (request_stage_ != RequestStage::Active) return;
    if (running_[request_].pin) {
        cache_.unpin_device(*running_[request_].pin);
        running_[request_].pin.reset();
    }
    if (host_pin_) {
        cache_.unpin_host(*host_pin_);
        host_pin_.reset();
    }
    const auto & observed = source_.observations();
    const auto & request = observed.requests[request_];
    const auto & tokens = observed.request_tokens.at(request.identity);
    const auto lookup = observed.token_paths.pages(tokens.lookup_input.value().path, page_tokens_);
    if (storage_ && !storage_->prepare(runtime, request_, lookup, cause_)) {
        wait_for_progress(runtime);
        return;
    }
    const auto matched = cache_.match(lookup);
    running_[request_].pin = matched.device;
    cache_.pin_device(*running_[request_].pin);

    // A pending host copy is not yet readable. Its completion re-enters this
    // decision; it cannot be converted into a zero-cost host hit.
    const auto host_pages = cache_.page_count(matched.host), device_pages = cache_.page_count(matched.device);
    if (host_pages > device_pages && (host_pages - device_pages) * page_tokens_ >= backend_.minimum_load_tokens) {
        // Protect the host source before device eviction can allocate another
        // backup and reclaim host pages. The transfer takes over this pin.
        host_pin_ = matched.host;
        cache_.pin_host(*host_pin_);
        if (cache_.ready_layers(matched.host, Tier::Host) != backend_.layers || !make_device_room(runtime, host_pages - device_pages)) {
            wait_for_progress(runtime);
            return;
        }
        const auto started = cache_.begin_load(matched.host);
        if (started.status == CopyStatus::SourceNotReady) {
            wait_for_progress(runtime);
            return;
        }
        if (started.status == CopyStatus::Started) {
            cache_.unpin_host(*host_pin_);
            host_pin_.reset();
            copy(runtime, *started.transfer, matched.host, CopyKind::Load, started.pages, false);
            wait_for_progress(runtime);
            return;
        }
    }
    if (cache_.ready_layers(matched.device, Tier::Device) != backend_.layers) {
        wait_for_progress(runtime);
        return;
    }
    const auto committed_tokens = observed.token_paths.length(tokens.committed.value().path);
    const auto active_pages = (committed_tokens + page_tokens_ - 1) / page_tokens_;
    if (!make_device_room(runtime, active_pages - device_pages)) {
        wait_for_progress(runtime);
        return;
    }
    running_[request_].buffer = cache_.reserve_request(active_pages - device_pages).value();
    requests_[request_].device_hit_pages = device_pages;
    requests_[request_].prefill_tokens = observed.token_paths.length(tokens.prompt.value().path) - device_pages * page_tokens_;
    waiting_ = false;
    compute(runtime);
}

void CacheProgram::compute(ProgramRuntime & runtime) {
    const auto & observed = source_.observations();
    const auto & request = observed.requests[request_];
    const auto frames = source_.request_frames(request.identity);
    if (frames.empty()) {
        issue_ = "missing_compute_frames";
        return;
    }
    if (std::ranges::none_of(frames, [&](source::Index frame) { return observed.compute_frames[frame].phase == Phase::Prefill; })) {
        issue_ = "missing_prefill_frame";
        return;
    }
    const auto & tokens = observed.request_tokens.at(request.identity);
    std::vector<FrameCompute> plans;
    plans.reserve(frames.size());
    for (const auto frame_id : frames) {
        const auto & frame = observed.compute_frames[frame_id];
        if (frame.requests.size() != 1 || frame.sequence_tokens.size() != 1 || !frame.query_tokens || frame.query_tokens->size() != 1) {
            issue_ = "unsupported_compute_frame";
            return;
        }
        const double query = frame.phase == Phase::Prefill ? requests_[request_].prefill_tokens : frame.query_tokens->front();
        const double context = frame.phase == Phase::Prefill ? observed.token_paths.length(tokens.prompt.value().path) : frame.sequence_tokens.front();
        const FrameInput variables{ query, query, context, 1, static_cast<double>(page_tokens_) };
        plans.push_back({ frame_id, variables, project_compute(source_, frame_id, variables) });
    }

    auto & compute = running_[request_].compute;
    compute = std::make_unique<ComputeProgram>(source_,
                                               std::move(plans),
                                               compute_resource_begin(),
                                               preparations_,
                                               queues_,
                                               ComputeScope::Request,
                                               ComputeProgram::first_owner + request_,
                                               this,
                                               request_);
    if (compute->issue()) {
        issue_ = compute->issue();
        for (const auto caller : running_[request_].callers) queues_.callers.release(runtime, caller, 0, *cause_);
        return;
    }
    // Admission and a data producer from an older request are separate causes.
    const auto ready = runtime.next_node();
    if (!submit(runtime, { 0, { { 0, Boundary{} } }, { { *requests_[request_].received, ready, graph::EdgeKind::ResourceOrder } } })) return;
    compute->start(runtime, ready, 0);
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

std::vector<Work> CacheProgram::begin_result_control(std::size_t request, std::size_t frame, ResultControlPoint point) {
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
    backup_prefix(runtime, insertion->leaf);
    if (waiting_) advance(runtime);
}

void CacheProgram::publish_prefill_prefix(ProgramRuntime & runtime, std::size_t request) {
    const auto & observed = source_.observations();
    const auto & tokens = observed.request_tokens.at(observed.requests[request].identity);
    const auto prompt = observed.token_paths.pages(tokens.prompt.value().path, page_tokens_);
    const auto leaf = cache_.commit_device(prompt, *running_[request].buffer).node;
    cache_.unpin_device(*running_[request].pin);
    running_[request].pin = leaf;
    cache_.pin_device(leaf);
    requests_[request].prefill_prefix_published = cause_;
    backup_prefix(runtime, leaf);
}

void CacheProgram::backup_prefix(ProgramRuntime & runtime, RadixId leaf) {
    std::vector<RadixId> path;
    for (auto node = leaf; node != 0; node = cache_.tree().nodes()[node].parent) path.push_back(node);
    for (auto at = path.rbegin(); at != path.rend(); ++at) {
        if (!cache_.backup_required(*at) || !make_host_room(cache_.tree().nodes()[*at].pages.size())) continue;
        const auto started = cache_.begin_backup(*at);
        if (started.status == CopyStatus::Started && !copy(runtime, *started.transfer, *at, CopyKind::Backup, started.pages, false)) return;
    }
}

void CacheProgram::commit(ProgramRuntime & runtime, std::size_t request) {
    auto & running = running_[request];
    const auto & observed = source_.observations();
    const auto & tokens = observed.request_tokens.at(observed.requests[request].identity);
    const auto committed = observed.token_paths.pages(tokens.committed.value().path, page_tokens_);
    if (!running.final_insertion) {
        if (!running.outputs_ready && !running.compute->complete()) running.final_publication = cache_.begin_device_publication();
        running.committed = cache_.commit_device(committed, *running.buffer, running.final_publication).node;
    }
    if (running.final_publication) requests_[request].deferred_kv_pages = cache_.publication_pages(*running.final_publication);
    // Existing ready pages need no dependency on an extra in-flight frame.
    if (running.final_publication && requests_[request].deferred_kv_pages == 0 && !running.final_insertion) {
        cache_.publish_all(*running.final_publication);
        cache_.acknowledge(*running.final_publication);
        running.final_publication.reset();
    }
    cache_.unpin_device(*running.pin);
    running.pin = running.committed;
    cache_.pin_device(*running.pin);
    if (!running.final_publication && !requests_[request].kv_published) requests_[request].kv_published = cause_;
    if (!running.final_insertion) backup_prefix(runtime, *running.committed);
    respond(runtime, request);
}

void CacheProgram::publish_result(ProgramRuntime & runtime, std::size_t request) {
    auto & running = running_[request];
    if (running.publication_scheduled) return;
    const auto end = runtime.next_node();
    PlannedMutation mutation{ 0, { { 0, Boundary{}, action(ActionKind::FinalPublished, request) } }, {} };
    if (running.outputs_ready) mutation.dependencies.push_back({ *running.outputs_ready, end, graph::EdgeKind::DataDependency });
    if (running.final_insertion && running.final_insertion->completed)
        mutation.dependencies.push_back({ *running.final_insertion->completed, end, graph::EdgeKind::DataDependency });
    running.publication_scheduled = submit(runtime, std::move(mutation));
}

bool CacheProgram::complete() const {
    const bool pending_insertion = std::ranges::any_of(running_, [](const RunningRequest & request) {
        return request.final_publication || (request.prefix_insertion && !request.prefix_insertion->completed)
               || (request.final_insertion && !request.final_insertion->completed);
    });
    if (pending_insertion || (!issue_ && request_ != requests_.size()) || !copies_.empty() || (storage_ && !storage_->idle())) return false;
    return issue_ || std::ranges::all_of(running_, [](const RunningRequest & request) { return !request.compute; });
}

void CacheProgram::on_event(ProgramRuntime & runtime, const simulation::Event & event) {
    if (event.owner == Storage::owner) {
        const auto changed = storage_->on_event(runtime, event);
        if (changed && waiting_) {
            cause_ = event.origin == simulation::EventOrigin::Completion ? std::optional(event.identity) : std::nullopt;
            advance(runtime);
        }
        return;
    }
    if (event.owner >= ComputeProgram::first_owner) {
        const auto request = event.owner - ComputeProgram::first_owner;
        auto & running = running_.at(request);
        running.compute->on_event(runtime, event);
        if (running.compute->issue()) {
            issue_ = running.compute->issue();
            return;
        }
        running.outputs_ready = running.compute->results_ready();
        if (!running.prefix_insertion && !requests_[request].prefill_prefix_published && running.compute->prefill_result_ready()) {
            cause_ = running.compute->prefill_result_ready();
            publish_prefill_prefix(runtime, request);
        }
        if (!requests_[request].response && running.compute->sent()) {
            cause_ = running.compute->sent();
            commit(runtime, request);
        }
        if (running.final_publication && running.outputs_ready && (!running.final_insertion || running.final_insertion->completed)) {
            cause_ = running.compute->results_ready();
            publish_result(runtime, request);
        }
        if (running.compute->complete()) {
            cause_ = event.identity;
            finish_compute(runtime, request);
            if (waiting_) advance(runtime);
        }
        return;
    }
    cause_ = event.identity;
    if (event.action.kind == std::to_underlying(ActionKind::PrefixInserted)) { complete_insertion(runtime, event.action.subject, ResultControl::UpdatePrefix); }
    else if (event.action.kind == std::to_underlying(ActionKind::FinalInserted)) {
        complete_insertion(runtime, event.action.subject, ResultControl::CommitPrefix);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::FinalPublished)) {
        const auto request = event.action.subject;
        auto & running = running_[request];
        cache_.publish_all(*running.final_publication);
        cache_.acknowledge(*running.final_publication);
        running.final_publication.reset();
        requests_[request].kv_published = event.identity;
        backup_prefix(runtime, *running.committed);
        if (waiting_) advance(runtime);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::ReleasedPages)) {
        auto & group = release_groups_[event.action.subject];
        group.completed = event.identity;
        auto & running = running_[group.request];
        std::size_t pages = 0;
        for (const auto & [point, count] : group.indices) pages += (count + page_tokens_ - 1) / page_tokens_;
        cache_.trim(*running.buffer, cache_.buffer_pages(*running.buffer) - pages);
        if (pages) {
            requests_[group.request].released_device_pages += pages;
            requests_[group.request].pages_released = event.identity;
        }
        finish_compute(runtime, group.request);
        if (waiting_) advance(runtime);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::Received)) {
        request_stage_ = RequestStage::Waiting;
        advance(runtime);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::AdmissionGranted)) {
        auto & running = running_[event.action.subject];
        running.admission.push_back(event.identity);
        if (running.admission.size() == running.callers.size()) {
            const auto begin = runtime.next_node();
            PlannedMutation mutation{ 0, { { 0, Boundary{}, action(ActionKind::Admitted, event.action.subject) } }, {} };
            for (const auto grant : running.admission) mutation.dependencies.push_back({ grant, begin, graph::EdgeKind::ResourceOrder });
            runtime.submit(std::move(mutation));
        }
    }
    else if (event.action.kind == std::to_underlying(ActionKind::Admitted)) {
        auto & received = requests_[event.action.subject].received;
        if (!received) received = event.identity;
        request_stage_ = RequestStage::Active;
        advance(runtime);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::ResponseDone)) {
        requests_[request_++].completed = true;
        request_stage_ = RequestStage::BeforeStart;
        advance(runtime);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::CopyDone)) {
        const auto transfer = event.action.subject;
        auto pending = std::move(copies_.at(transfer));
        // This backend copies every layer in one DMA; publish once after all
        // participating copies, then acknowledge and release their pins.
        cache_.publish_all(transfer);
        cache_.acknowledge(transfer);
        if (storage_ && pending.kind == CopyKind::Backup) storage_->enqueue_write(runtime, pending.node, std::move(pending.storage_pages), event.identity);
        copies_.erase(transfer);
        if (pending.kind == CopyKind::Load) requests_[request_].loaded_pages += pending.pages;
        if (pending.evict_after) {
            pending_evictions_.push_back(pending.node);
            return;
        }
        if (waiting_) advance(runtime);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::EvictionDone)) {
        eviction_wait_ = false;
        // The explicit join preserves the whole backup batch in the DAG,
        // instead of depending only on whichever rank happened to finish last.
        for (const auto node : pending_evictions_) {
            if (cache_.evict_device(node) == Eviction::Removed) ++device_evictions_;
        }
        pending_evictions_.clear();
        advance(runtime);
    }
}

} // namespace markov::trace_graph::planning
