#include "markov/trace_graph/planning/cache_program.hpp"

#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/planning/compute_program.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>

namespace markov::trace_graph::planning {

CacheProgram::CacheProgram(const source::SourceContext & source, std::size_t page_tokens, CacheConfig config, CacheBackend backend)
    : source_(source),
      page_tokens_(page_tokens),
      cache_(config),
      backend_(std::move(backend)),
      requests_(source.observations().requests.size()) {}

void CacheProgram::record_peak() {
    peak_device_pages_ = std::max(peak_device_pages_, cache_.used_pages(Tier::Device));
    peak_host_pages_ = std::max(peak_host_pages_, cache_.used_pages(Tier::Host));
}

bool CacheProgram::make_host_room(std::size_t pages) {
    for (const auto node : cache_.eviction_order(Tier::Host)) {
        if (cache_.available_pages(Tier::Host) >= pages) break;
        if (cache_.evict_host(node)) ++host_evictions_;
    }
    return cache_.available_pages(Tier::Host) >= pages;
}

bool CacheProgram::submit(ProgramRuntime & runtime, PlannedMutation mutation) {
    if (cause_) {
        const auto begin = runtime.next_node();
        std::vector<bool> has_input(mutation.operations.size(), false);
        for (const auto & edge : mutation.dependencies) has_input[edge.to - begin] = true;
        for (std::size_t id = 0; id < has_input.size(); ++id) {
            if (!has_input[id]) mutation.dependencies.push_back({ *cause_, begin + id, graph::EdgeKind::DataDependency });
        }
    }
    return runtime.submit(std::move(mutation)).has_value();
}

bool CacheProgram::copy(ProgramRuntime & runtime, TransferId transfer, RadixId node, CopyKind kind, std::size_t pages, bool evict_after) {
    record_peak();
    const double page_bytes = page_tokens_ * backend_.bytes_per_token;
    const Work work{
        kind == CopyKind::Load ? backend_.load : backend_.backup,
        { { "bytes", pages * page_bytes }, { "operations", 1 }, { "page_bytes", page_bytes } }
    };
    const auto begin = runtime.next_node();
    copies_.emplace(transfer, PendingCopy{ node, evict_after, kind, pages, begin + backend_.participants });
    PlannedMutation mutation{ 0, {}, {} };
    for (std::size_t rank = 0; rank < backend_.participants; ++rank) {
        const auto resource = source_.observations().resources.size() + 2 * rank + (kind == CopyKind::Load ? 0 : 1);
        mutation.operations.push_back({ resource, std::cref(work) });
        mutation.dependencies.push_back({ begin + rank, begin + backend_.participants, graph::EdgeKind::DataDependency });
    }
    mutation.operations.push_back({
        0,
        Boundary{},
        simulation::Action{ CopyDone, transfer }
    });
    const auto submitted = submit(runtime, std::move(mutation));
    if (submitted) kind == CopyKind::Load ? ++loads_ : ++backups_;
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
        const auto end = runtime.next_node();
        PlannedMutation batch{ 0, { { 0, Boundary{}, simulation::Action{ EvictionDone, request_ } } }, {} };
        for (const auto completion : backups) batch.dependencies.push_back({ completion, end, graph::EdgeKind::DataDependency });
        submit(runtime, std::move(batch));
        return false;
    }
    if (cache_.available_pages(Tier::Device) >= pages) return true;
    if (copies_.empty()) issue_ = "device_capacity_unavailable";
    return false;
}

void CacheProgram::start(ProgramRuntime & runtime) { advance(runtime); }

void CacheProgram::advance(ProgramRuntime & runtime) {
    if (issue_ || request_ == requests_.size()) return;
    waiting_ = true;
    for (const auto & barrier : source_.observations().workload_barriers) {
        if (barrier.before_request == request_ && !copies_.empty()) return;
    }
    if (request_pin_) {
        cache_.unpin_device(*request_pin_);
        request_pin_.reset();
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
    request_pin_ = matched.device;
    cache_.pin_device(*request_pin_);

    // A pending host copy is not yet readable. Its completion re-enters this
    // decision; it cannot be converted into a zero-cost host hit.
    const auto host_pages = cache_.page_count(matched.host), device_pages = cache_.page_count(matched.device);
    if (host_pages > device_pages && (host_pages - device_pages) * page_tokens_ >= backend_.minimum_load_tokens) {
        // Protect the host source before device eviction can allocate another
        // backup and reclaim host pages. The transfer takes over this pin.
        host_pin_ = matched.host;
        cache_.pin_host(*host_pin_);
        if (cache_.ready_layers(matched.host, Tier::Host) != backend_.layers) return;
        if (!make_device_room(runtime, host_pages - device_pages)) return;
        const auto started = cache_.begin_load(matched.host);
        if (started.status == CopyStatus::Started) {
            cache_.unpin_host(*host_pin_);
            host_pin_.reset();
            copy(runtime, *started.transfer, matched.host, CopyKind::Load, started.pages, false);
            return;
        }
    }
    if (cache_.ready_layers(matched.device, Tier::Device) != backend_.layers) return;
    const auto committed_tokens = observed.token_paths.length(tokens.committed.value().path);
    const auto active_pages = (committed_tokens + page_tokens_ - 1) / page_tokens_;
    if (!make_device_room(runtime, active_pages - device_pages)) return;
    buffer_ = cache_.reserve(Tier::Device, active_pages - device_pages).value();
    record_peak();
    requests_[request_].device_hit_pages = device_pages;
    requests_[request_].prefill_tokens = observed.token_paths.length(tokens.prompt.value().path) - device_pages * page_tokens_;
    waiting_ = false;
    compute(runtime, Phase::Prefill);
}

void CacheProgram::compute(ProgramRuntime & runtime, Phase phase) {
    phase_ = phase;
    const auto & observed = source_.observations();
    const auto & request = observed.requests[request_];
    const auto frames = source_.request_frames(request.identity);
    if (frames.empty()) {
        issue_ = "missing_compute_frames";
        return;
    }
    const auto & tokens = observed.request_tokens.at(request.identity);
    std::vector<ComputePlan> plans;
    plans.reserve(frames.size());
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ 0, {}, {} };
    std::vector<graph::NodeId> completions;
    for (const auto frame_id : frames) {
        const auto & frame = observed.compute_frames[frame_id];
        if (frame.phase != phase) continue;
        if (frame.requests.size() != 1 || frame.sequence_tokens.size() != 1 || !frame.query_tokens || frame.query_tokens->size() != 1) {
            issue_ = "unsupported_compute_frame";
            return;
        }
        const double query = phase == Phase::Prefill ? requests_[request_].prefill_tokens : frame.query_tokens->front();
        const double context = phase == Phase::Prefill ? observed.token_paths.length(tokens.prompt.value().path) : frame.sequence_tokens.front();
        plans.push_back(project_compute(source_, frame_id, { query, query, context, 1, static_cast<double>(page_tokens_) }));
        const auto & plan = plans.back();
        if (!plan.missing_row_bindings.empty() || !source_.frame_queue_issues(frame_id).empty()) {
            issue_ = "unresolved_compute_work_or_queue";
            return;
        }
        auto frame_work = compute_mutation(source_, plan, frame_id, begin + mutation.operations.size(), 0, std::nullopt);
        mutation.operations.insert(mutation.operations.end(),
                                   std::make_move_iterator(frame_work.operations.begin()),
                                   std::make_move_iterator(frame_work.operations.end()));
        mutation.dependencies.insert(mutation.dependencies.end(), frame_work.dependencies.begin(), frame_work.dependencies.end());
        completions.push_back(begin + mutation.operations.size() - 1);
    }
    if (completions.empty()) {
        if (phase == Phase::Prefill) issue_ = "missing_prefill_frame";
        else commit(runtime);
        return;
    }
    const auto end = begin + mutation.operations.size();
    mutation.operations.push_back({
        0,
        Boundary{},
        simulation::Action{ PhaseDone, request_ }
    });
    for (const auto completion : completions) mutation.dependencies.push_back({ completion, end, graph::EdgeKind::DataDependency });
    submit(runtime, std::move(mutation));
}

void CacheProgram::commit(ProgramRuntime & runtime) {
    const auto & observed = source_.observations();
    const auto & tokens = observed.request_tokens.at(observed.requests[request_].identity);
    const auto committed = observed.token_paths.pages(tokens.committed.value().path, page_tokens_);
    const auto leaf = cache_.commit_device(committed, *buffer_);
    cache_.release(*buffer_);
    buffer_.reset();
    cache_.unpin_device(*request_pin_);
    request_pin_.reset();

    std::vector<RadixId> path;
    for (auto node = leaf; node != 0; node = cache_.tree().nodes()[node].parent) path.push_back(node);
    for (auto at = path.rbegin(); at != path.rend(); ++at) {
        if (!cache_.backup_required(*at) || !make_host_room(cache_.tree().nodes()[*at].pages.size())) continue;
        const auto started = cache_.begin_backup(*at);
        if (started.status == CopyStatus::Started && !copy(runtime, *started.transfer, *at, CopyKind::Backup, started.pages, false)) return;
    }
    requests_[request_++].completed = true;
    advance(runtime);
}

void CacheProgram::on_event(ProgramRuntime & runtime, const simulation::Event & event) {
    cause_ = event.identity;
    if (event.action.kind == CopyDone) {
        const auto transfer = event.action.subject;
        const auto pending = copies_.at(transfer);
        // This backend copies every layer in one DMA; publish once after all
        // participating copies, then acknowledge and release their pins.
        cache_.publish_all(transfer);
        cache_.acknowledge(transfer);
        copies_.erase(transfer);
        if (pending.kind == CopyKind::Load) requests_[request_].loaded_pages += pending.pages;
        if (pending.evict_after) {
            pending_evictions_.push_back(pending.node);
            return;
        }
        if (waiting_) advance(runtime);
    }
    else if (event.action.kind == EvictionDone) {
        // The explicit join preserves the whole backup batch in the DAG,
        // instead of depending only on whichever rank happened to finish last.
        for (const auto node : pending_evictions_) {
            if (cache_.evict_device(node) == Eviction::Removed) ++device_evictions_;
        }
        pending_evictions_.clear();
        advance(runtime);
    }
    else if (!issue_) {
        if (phase_ == Phase::Prefill) compute(runtime, Phase::Decode);
        else commit(runtime);
    }
}

} // namespace markov::trace_graph::planning
