#include "markov/trace_graph/planning/cache_program.hpp"

#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/planning/compute_program.hpp"
#include "markov/trace_graph/source/regions.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <utility>

namespace markov::trace_graph::planning {

CacheProgram::CacheProgram(const source::SourceContext & source, std::size_t page_tokens, CacheConfig config, CacheBackend backend,
                           std::optional<StorageConfig> storage, std::optional<KernelCacheInitial> kernel_cache_initial, std::optional<RegionBackend> regions)
    : source_(source),
      page_tokens_(page_tokens),
      cache_(config),
      backend_(std::move(backend)),
      preparations_(kernel_cache_initial),
      regions_(std::move(regions)),
      region_preparation_(kernel_cache_initial),
      running_(source.observations().requests.size()),
      requests_(source.observations().requests.size()) {
    if (std::ranges::any_of(source.observations().requests, [](const source::RequestWindow & request) { return !request.received || !request.sent; })) {
        issue_ = "request_boundaries_unavailable";
        return;
    }
    for (std::size_t request = 0; request < requests_.size(); ++request) {
        std::set<std::size_t> callers;
        for (const auto frame : source.request_frames(source.observations().requests[request].identity)) {
            const auto operations = source.frame_operations(frame);
            if (!operations.empty()) callers.insert(source.observations().work_observations[operations.front()].resource);
            if (regions_ && source.observations().compute_frames[frame].phase == Phase::Prefill) {
                const auto * result = source.frame_result(frame);
                const auto gap = source.regions().frame(frame).before_preparation;
                if (result && gap) running_[request].admission_gaps.try_emplace(result->run.resource, *gap);
            }
        }
        if (callers.size() != backend_.participants) {
            issue_ = "request_caller_resources_unavailable";
            return;
        }
        running_[request].callers.assign(callers.begin(), callers.end());
        if (regions_ && running_[request].admission_gaps.size() != callers.size()) {
            issue_ = "request_admission_regions_unavailable";
            return;
        }
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
    if (cache_.available_pages(Tier::Host) < pages) host_evicted_pages_ += cache_.evict_host_pages(pages);
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

bool CacheProgram::copy(ProgramRuntime & runtime, TransferId transfer, RadixId node, CopyKind kind, std::size_t pages, bool evict_after, std::size_t request) {
    if (regions_) return region_copy(runtime, transfer, node, kind, pages, evict_after, request);
    if (kind == CopyKind::Backup && evict_after) return eviction_backup(runtime, transfer, node, pages);
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
        const auto previous = queues_.native.find(resource);
        if (previous != queues_.native.end()) mutation.dependencies.push_back({ previous->second, begin + rank, graph::EdgeKind::ResourceOrder });
        mutation.dependencies.push_back({ begin + rank, begin + backend_.participants, graph::EdgeKind::DataDependency });
    }
    mutation.operations.push_back({ 0, Boundary{}, action(ActionKind::CopyDone, transfer) });
    const auto submitted = submit(runtime, std::move(mutation));
    if (submitted) {
        for (std::size_t rank = 0; rank < backend_.participants; ++rank)
            queues_.native[source_.observations().resources.size() + 2 * rank + (kind == CopyKind::Load ? 0 : 1)] = begin + rank;
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
                if (!copy(runtime, *started.transfer, node, CopyKind::Backup, started.pages, true, request_)) return false;
                reclaiming += started.pages;
                backups.push_back(copies_.at(*started.transfer).completion.value());
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
    const auto resource = source_.client_resource();
    const auto begin = runtime.next_node();
    PlannedMutation mutation{
        0,
        { { resource, Boundary{} }, { resource, SourceClientDelay{ request_, false }, action(ActionKind::Received, request_) } },
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
    const auto resource = source_.client_resource();
    const auto response = runtime.next_node();
    if (!submit(runtime, { 0, { { resource, SourceClientDelay{ request, true }, action(ActionKind::ResponseDone, request) } }, {} })) return;
    requests_[request].response = response;
    request_stage_ = RequestStage::Response;
    waiting_ = false;
}

void CacheProgram::wait_for_progress(ProgramRuntime & runtime) {
    // Write-back eviction synchronizes its copies inside the caller turn.
    if (eviction_wait_) return;
    // A regional load keeps its admission turn until the source is readable.
    if (regions_ && std::ranges::any_of(copies_, [](const auto & copy) { return copy.second.kind == CopyKind::Load; })) return;
    // A blocked admission yields the caller so older results can release space.
    for (const auto caller : running_[request_].callers) queues_.callers.release(runtime, caller, 0, *cause_);
    running_[request_].admission.clear();
    running_[request_].queried = false;
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
    const auto matched = cache_.match(lookup);
    if (!running_[request_].queried) {
        query(runtime, lookup, matched);
        return;
    }
    if (storage_) {
        storage_->begin_prefetch(runtime, request_, lookup, matched, cause_);
        if (!storage_->check_prefetch(runtime, request_, cause_)) {
            wait_for_progress(runtime);
            return;
        }
    }
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
            copy(runtime, *started.transfer, matched.host, CopyKind::Load, started.pages, false, request_);
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

void CacheProgram::query(ProgramRuntime & runtime, std::span<const PrefixId> lookup, const CacheMatch & matched) {
    const auto & observed = source_.observations();
    const auto & request = observed.requests[request_];
    std::vector<Work> copies;
    for (const auto & copy : matched.index_copies)
        copies.push_back({
            { "cache.index_clone", {}, {} },
            { { "pages", copy.pages }, { "host", copy.tier == Tier::Host ? 1 : 0 } }
        });
    std::size_t segments = 0;
    for (auto node = matched.device; node != 0; node = cache_.tree().nodes()[node].parent)
        if (cache_.node(node).device) ++segments;
    const std::vector<Work> match{
        { { "cache.index_match", {}, {} }, { { "pages", cache_.page_count(matched.device) }, { "input_tensors", segments } } }
    };
    auto & running = running_[request_];
    running.query_submissions.clear();
    request_stage_ = RequestStage::Querying;
    requests_[request_].query_index_copies += copies.size();
    if (regions_) {
        if (storage_) storage_->begin_prefetch(runtime, request_, lookup, matched, cause_);
        const auto begin = runtime.next_node();
        PlannedMutation mutation{ 0, {}, {} };
        for (const auto caller : running.callers) {
            const auto node = begin + mutation.operations.size();
            PlannedOperation control{ caller, Boundary{}, action(ActionKind::QuerySubmitted, request_) };
            const auto gap = running.admission_gaps.find(caller);
            if (gap != running.admission_gaps.end()) control.cost = SourceRegionGap{ gap->second };
            mutation.operations.push_back(std::move(control));
            mutation.operations.push_back({ caller, Boundary{}, action(ActionKind::QueryDone, request_) });
            mutation.dependencies.push_back({ node, node + 1, graph::EdgeKind::DataDependency });
        }

        // Keep the first control residual before admission, where prefetch can
        // progress. RegionCompute must not charge that same interval again.
        if (submit(runtime, std::move(mutation))) {
            pending_queries_ += running.callers.size();
            compute_stats_.source_cpu_gaps += running.admission_gaps.size();
            running.admission_gaps.clear();
        }
        return;
    }
    for (const auto frame_id : source_.request_frames(request.identity)) {
        const auto & frame = observed.compute_frames[frame_id];
        if (frame.phase != Phase::Prefill) continue;
        if (frame.requests.size() != 1 || frame.sequence_tokens.size() != 1 || !frame.query_tokens || frame.query_tokens->size() != 1) {
            issue_ = "unsupported_compute_frame";
            return;
        }
        const auto sequences = source_.lookup_sequences(frame_id);
        const auto operations = source_.frame_operations(frame_id);
        if (sequences.size() != 2 || operations.empty() || !source_.compute_resources(operations.front())) {
            issue_ = "cache_lookup_recipe_or_queues_unavailable";
            return;
        }
        const FrameInput input{ frame.query_tokens->front(), frame.query_tokens->front(), frame.sequence_tokens.front(), 1, static_cast<double>(page_tokens_) };
        NativePlan plan;
        for (std::size_t at = 0; at < sequences.size(); ++at) {
            const auto & site = source_.sequence(sequences[at]);
            auto calls = site.observation.definition->describe(Phase::Prefill, input, at == 0 ? copies : match);
            if (calls.issue) {
                issue_ = calls.issue;
                return;
            }
            auto placement = site.resources.unique_resources();
            if (!placement) {
                if (!site.resources.workers.empty() || !site.resources.devices.empty()) {
                    issue_ = "cache_lookup_queues_ambiguous";
                    return;
                }
                placement = source_.compute_resources(operations.front());
            }
            source_.bind_native_sources(sequences[at], calls.calls, *placement);
            if (!plan.extend(calls, *placement, preparations_)) {
                issue_ = "kernel_cache_initial_state_unavailable";
                return;
            }
        }
        const auto begin = runtime.next_node();
        const NativeEvents events{ action(ActionKind::QueryDone, request_), action(ActionKind::QuerySubmitted, request_) };
        if (!runtime.submit(plan.mutation(0, begin, events, cause_, queues_.native))) return;
        for (const auto & [resource, node] : plan.frontier(begin)) queues_.native[resource] = node;
        requests_[request_].query_operations += plan.operations();
        ++pending_queries_;
    }
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
    std::vector<RegionFrame> region_frames;
    plans.reserve(frames.size());
    for (const auto frame_id : frames) {
        const auto & frame = observed.compute_frames[frame_id];
        if (frame.requests.size() != 1 || frame.sequence_tokens.size() != 1 || !frame.query_tokens || frame.query_tokens->size() != 1) {
            issue_ = "unsupported_compute_frame";
            return;
        }
        const double query = frame.phase == Phase::Prefill ? requests_[request_].prefill_tokens : frame.query_tokens->front();
        if (regions_ && frame.phase == Phase::Prefill && query > regions_->maximum_prefill_tokens) {
            issue_ = "target_requires_prefill_chunking";
            return;
        }
        const double context = frame.phase == Phase::Prefill ? observed.token_paths.length(tokens.prompt.value().path) : frame.sequence_tokens.front();
        const FrameInput variables{ query, query, context, 1, static_cast<double>(page_tokens_) };
        if (regions_) region_frames.push_back({ frame_id, variables });
        else plans.push_back({ frame_id, variables, project_compute(source_, frame_id, variables) });
    }

    auto & compute = running_[request_].compute;
    if (regions_)
        compute = std::make_unique<RegionCompute>(source_,
                                                  std::move(region_frames),
                                                  *regions_,
                                                  compute_resource_begin(),
                                                  region_preparation_,
                                                  queues_,
                                                  *this,
                                                  request_);
    else
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

void CacheProgram::publish_prefill_prefix(ProgramRuntime & runtime, std::size_t request) {
    const auto & observed = source_.observations();
    const auto & tokens = observed.request_tokens.at(observed.requests[request].identity);
    const auto prompt = observed.token_paths.pages(tokens.prompt.value().path, page_tokens_);
    const auto leaf = cache_.commit_device(prompt, *running_[request].buffer).node;
    cache_.unpin_device(*running_[request].pin);
    running_[request].pin = leaf;
    cache_.pin_device(leaf);
    requests_[request].prefill_prefix_published = cause_;
    backup_prefix(runtime, leaf, request);
}

void CacheProgram::backup_prefix(ProgramRuntime & runtime, RadixId leaf, std::size_t request) {
    std::vector<RadixId> path;
    for (auto node = leaf; node != 0; node = cache_.tree().nodes()[node].parent) path.push_back(node);
    for (auto at = path.rbegin(); at != path.rend(); ++at) {
        if (!cache_.backup_required(*at) || !make_host_room(cache_.tree().nodes()[*at].pages.size())) continue;
        const auto started = cache_.begin_backup(*at);
        if (started.status == CopyStatus::Started && !copy(runtime, *started.transfer, *at, CopyKind::Backup, started.pages, false, request)) return;
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
    if (!running.final_insertion) backup_prefix(runtime, *running.committed, request);
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
    if (pending_queries_ || pending_insertion || (!issue_ && request_ != requests_.size()) || !copies_.empty() || (storage_ && !storage_->idle())) return false;
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
        if (running.final_insertion) {
            running.compute->resume_controls(runtime, event.identity);
            if (running.compute->issue()) issue_ = running.compute->issue();
        }
        else backup_prefix(runtime, *running.committed, request);
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
    else if (event.action.kind == std::to_underlying(ActionKind::QueryDone)) { --pending_queries_; }
    else if (event.action.kind == std::to_underlying(ActionKind::QuerySubmitted)) {
        auto & running = running_[event.action.subject];
        running.query_submissions.push_back(event.identity);
        if (running.query_submissions.size() == backend_.participants) {
            const auto begin = runtime.next_node();
            PlannedMutation mutation{ 0, { { 0, Boundary{}, action(ActionKind::Admitted, event.action.subject) } }, {} };
            for (const auto submitted : running.query_submissions) mutation.dependencies.push_back({ submitted, begin, graph::EdgeKind::ResourceOrder });
            running.queried = true;
            runtime.submit(std::move(mutation));
        }
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
    else if (event.action.kind == std::to_underlying(ActionKind::CopyParticipantDone)) {
        copies_.at(event.action.subject).copy_completions.push_back(event.identity);
        join_copy(runtime, event.action.subject);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::CopyGranted)) { submit_region_copy(runtime, event.action.subject, event.identity); }
    else if (event.action.kind == std::to_underlying(ActionKind::CopyReturned)) {
        const auto & turn = region_copy_turns_[event.action.subject];
        queues_.callers.release(runtime, running_[turn.request].callers[turn.rank], 0, event.identity);
    }
    else if (event.action.kind == std::to_underlying(ActionKind::CopyDone)) {
        const auto transfer = event.action.subject;
        auto pending = std::move(copies_.at(transfer));
        // Publish the all-layer batch after every participant and retained caller
        // has completed; then acknowledge and release the source pins.
        cache_.publish_all(transfer);
        cache_.acknowledge(transfer);
        completed_copies_.push_back({ pending.kind, pending.pages, std::move(pending.caller_returns), std::move(pending.copy_completions), event.identity });
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
