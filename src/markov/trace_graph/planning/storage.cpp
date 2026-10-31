#include "markov/trace_graph/planning/storage.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::planning {

Storage::Storage(Cache & cache, const TokenPaths & paths, std::size_t page_tokens, std::size_t bytes_per_token, std::size_t participants,
                 std::size_t resource_begin, std::size_t requests, StorageConfig config, StorageBackend backend)
    : cache_(cache),
      paths_(paths),
      page_tokens_(page_tokens),
      page_bytes_(page_tokens * bytes_per_token),
      participants_(participants),
      resource_begin_(resource_begin),
      config_(config),
      backend_(std::move(backend)),
      results_(requests),
      workers_(participants) {}

bool Storage::service(ProgramRuntime & runtime, const WorkKind & kind, std::size_t pages, std::size_t rank, bool writer, simulation::Action completion,
                      std::optional<graph::NodeId> cause) {
    const Work work{
        kind,
        { { "bytes", static_cast<double>(pages * page_bytes_) },
          { "pages", static_cast<double>(pages) },
          { "operations", 1 },
          { "page_bytes", static_cast<double>(page_bytes_) } }
    };
    const auto node = runtime.next_node();
    PlannedMutation mutation{ owner, { { resource_begin_ + 2 * rank + writer, std::cref(work), completion } }, {} };
    if (runtime.records_graph()) {
        if (writer) {
            const auto & cursor = workers_[rank].writes.front();
            const auto & operation = writes_.at(cursor.operation);
            const auto page = kind == backend_.new_page || kind == backend_.existing_page ? cursor.index : cursor.batch_end - 1;
            mutation.operations.front().position =
                OperationPosition{ std::nullopt, cursor.operation, rank, "storage_write", paths_.suffix(operation.pages[page], 0) };
        }
        else {
            const auto request = completion.subject / participants_;
            const auto & operation = prefetches_.at(request);
            const auto & cursor = operation.workers[rank];
            auto page = cursor.batch_end - 1;
            if (kind == backend_.read_page) page = cursor.read_index;
            else if (kind == backend_.copy_publish) page = cursor.copy_index;
            mutation.operations.front().position = OperationPosition{ request, 0, rank, "storage_read", paths_.suffix(operation.keys[page], 0) };
        }
    }
    if (cause) mutation.dependencies.push_back({ *cause, node, graph::EdgeKind::DataDependency });
    return runtime.submit(std::move(mutation)).has_value();
}

graph::NodeId Storage::join(ProgramRuntime & runtime, std::span<const graph::NodeId> causes) {
    const auto node = runtime.next_node();
    PlannedMutation mutation{ owner, { { 0, Boundary{} } }, {} };
    for (const auto cause : causes) mutation.dependencies.push_back({ cause, node, graph::EdgeKind::Synchronization });
    runtime.submit(std::move(mutation));
    return node;
}

void Storage::begin_prefetch(ProgramRuntime & runtime, std::size_t request, std::span<const PrefixId> lookup, const CacheMatch & matched,
                             std::optional<graph::NodeId> cause) {
    auto & result = results_[request];
    if (result.examined) return;
    result.examined = true;
    const auto host_pages = cache_.page_count(matched.host);
    const auto start = std::max(host_pages, cache_.page_count(matched.device));
    const auto requested = lookup.size() - start;
    if (requested * page_tokens_ < config_.threshold_tokens
        || occupied_pages_ * page_tokens_ >= static_cast<std::size_t>(config_.prefetch_limit_pages * page_tokens_)) {
        result.ready = result.worker_done = true;
        return;
    }
    cache_.pin_host(matched.host);
    if (cache_.available_pages(Tier::Host) < requested) stats_.host_evicted_pages += cache_.evict_host_pages(requested);
    const auto count = std::min(requested, cache_.available_pages(Tier::Host));
    if (count * page_tokens_ < config_.threshold_tokens) {
        cache_.unpin_host(matched.host);
        result.ready = result.worker_done = true;
        return;
    }
    const auto buffer = cache_.reserve(Tier::Host, count).value();
    stats_.peak_reserved_pages = std::max(stats_.peak_reserved_pages, reserved_pages_ + count);
    ++stats_.queries;
    result.requested_pages = count;
    result.rebased_suffix = start != host_pages;

    // Native hashing starts at the host anchor but skips the device prefix.
    // Resolve that actual token chain; do not silently assume the full prompt key.
    const auto tokens = paths_.suffix(lookup.back(), start * page_tokens_);
    auto anchor = host_pages ? lookup[host_pages - 1] : PrefixId{ 0 };
    std::vector<PrefixId> keys;
    for (std::size_t page = 0; page < count; ++page) {
        const auto next = paths_.append_known(anchor, std::span(tokens).subspan(page * page_tokens_, page_tokens_));
        if (!next || !std::ranges::all_of(workers_, [&](const Worker & worker) { return worker.files.contains(*next); })) break;
        keys.push_back(*next);
        anchor = *next;
    }
    result.queried_pages = keys.size();
    if (keys.size() * page_tokens_ < config_.threshold_tokens) {
        cache_.release(buffer);
        cache_.unpin_host(matched.host);
        result.ready = result.worker_done = true;
        return;
    }
    cache_.trim(buffer, keys.size());
    Prefetch operation{};
    operation.prefix.assign(lookup.begin(), lookup.begin() + host_pages);
    operation.keys = std::move(keys);
    operation.workers.resize(participants_);
    operation.buffer = buffer;
    operation.anchor = matched.host;
    operation.enqueue_cause = cause;
    reserved_pages_ += operation.keys.size();
    occupied_pages_ += count;
    prefetches_.emplace(request, std::move(operation));
    ++stats_.prefetches;
    result.issued_ns = runtime.now_ns();
    for (std::size_t rank = 0; rank < participants_; ++rank) {
        auto & worker = workers_[rank];
        worker.reads.push_back(request);
        if (worker.reads.size() == 1) start_read(runtime, request, rank, worker.read_completion);
    }

    if (config_.policy == PrefetchPolicy::Timeout) {
        const auto budget = std::min<long double>(
            config_.timeout_max_ns,
            config_.timeout_base_ns + static_cast<long double>(config_.timeout_per_ki_token_ns) * result.queried_pages * page_tokens_ / 1'024);
        if (budget >= static_cast<long double>(std::numeric_limits<graph::Time>::max()) - runtime.now_ns())
            throw std::runtime_error("Prefetch deadline exceeds the execution clock range");

        // The native condition is elapsed > budget, hence the next integer ns.
        prefetches_.at(request).timer =
            runtime.schedule_at(runtime.now_ns() + static_cast<graph::Time>(std::floor(budget)) + 1, owner, action(ActionKind::Timeout, request));
    }
}

PrefetchCheck Storage::check_prefetch(ProgramRuntime & runtime, std::size_t request, std::optional<graph::NodeId> cause) {
    auto & result = results_[request];
    if (!result.first_check_ns) result.first_check_ns = runtime.now_ns();
    if (result.ready) return PrefetchCheck::Ready;

    const auto & operation = prefetches_.at(request);
    const auto complete =
        result.published_pages == operation.keys.size() || std::ranges::any_of(operation.workers, [](const ReadCursor & worker) { return worker.terminated; });
    if (operation.stopping || complete || config_.policy == PrefetchPolicy::BestEffort || operation.deadline_cause) {
        stop(runtime, request, cause);
        return PrefetchCheck::Finalizing;
    }
    return PrefetchCheck::Pending;
}

void Storage::start_read(ProgramRuntime & runtime, std::size_t request, std::size_t rank, std::optional<graph::NodeId> cause) {
    auto & operation = prefetches_.at(request);
    auto & cursor = operation.workers[rank];
    cursor.batch_end = std::min(cursor.batch_begin + backend_.batch_pages, operation.keys.size());
    cursor.read_index = cursor.copy_index = cursor.batch_begin;
    cursor.readable.assign(cursor.batch_end - cursor.batch_begin, false);
    service(runtime,
            backend_.allocate,
            cursor.batch_end - cursor.batch_begin,
            rank,
            false,
            action(ActionKind::Allocated, request * participants_ + rank),
            queued_cause(runtime, operation.enqueue_cause, cause));
}

std::optional<graph::NodeId> Storage::queued_cause(ProgramRuntime & runtime, std::optional<graph::NodeId> enqueue, std::optional<graph::NodeId> previous) {
    if (!enqueue) return previous;
    if (!previous || previous == enqueue) return enqueue;
    const auto node = runtime.next_node();
    return runtime.submit({
        owner,
        { { 0, Boundary{} } },
        { { *enqueue, node, graph::EdgeKind::Submission }, { *previous, node, graph::EdgeKind::ResourceOrder } }
    });
}

void Storage::touch(std::size_t rank, PrefixId key) {
    auto & files = workers_[rank].files;
    const auto found = files.find(key);
    if (found != files.end()) found->second = ++access_;
}

void Storage::read_page(ProgramRuntime & runtime, std::size_t request, std::size_t rank, graph::NodeId cause) {
    auto & operation = prefetches_.at(request);
    auto & cursor = operation.workers[rank];
    while (cursor.read_index < cursor.batch_end) {
        const auto page = cursor.read_index;
        if (workers_[rank].files.contains(operation.keys[page])) {
            cursor.readable[page - cursor.batch_begin] = true;
            service(runtime, backend_.read_page, 1, rank, false, action(ActionKind::PageRead, request * participants_ + rank), cause);
            return;
        }
        // A missing file has no payload transfer. Its metadata work is not a full read.
        ++cursor.read_index;
    }
    copy_page(runtime, request, rank, cause);
}

void Storage::copy_page(ProgramRuntime & runtime, std::size_t request, std::size_t rank, graph::NodeId cause) {
    auto & cursor = prefetches_.at(request).workers[rank];
    if (cursor.copy_index == cursor.batch_end || !cursor.readable[cursor.copy_index - cursor.batch_begin]) {
        if (cursor.copy_index != cursor.batch_end) cursor.terminated = true;
        return_read(runtime, request, rank, cause);
        return;
    }
    // Native cancellation rejects publication after one already-started copy.
    service(runtime, backend_.copy_publish, 1, rank, false, action(ActionKind::PageCopied, request * participants_ + rank), cause);
}

void Storage::return_read(ProgramRuntime & runtime, std::size_t request, std::size_t rank, graph::NodeId cause) {
    const auto & cursor = prefetches_.at(request).workers[rank];
    service(runtime,
            backend_.read_return,
            cursor.batch_end - cursor.batch_begin,
            rank,
            false,
            action(ActionKind::ReadReturned, request * participants_ + rank),
            cause);
}

void Storage::stop(ProgramRuntime & runtime, std::size_t request, std::optional<graph::NodeId> cause) {
    auto & operation = prefetches_.at(request);
    if (operation.stopping) return;
    operation.stopping = true;
    for (auto & cursor : operation.workers) cursor.terminated = true;
    if (operation.timer) runtime.cancel_timer(*operation.timer);
    auto & result = results_[request];
    result.stopped_early = result.published_pages < operation.keys.size();
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ owner, { { 0, Boundary{}, action(ActionKind::Accept, request) } }, {} };
    if (cause) mutation.dependencies.push_back({ *cause, begin, graph::EdgeKind::DataDependency });
    if (operation.enqueue_cause) mutation.dependencies.push_back({ *operation.enqueue_cause, begin, graph::EdgeKind::DataDependency });
    if (operation.publication_cause) mutation.dependencies.push_back({ *operation.publication_cause, begin, graph::EdgeKind::DataDependency });
    if (operation.deadline_cause) mutation.dependencies.push_back({ *operation.deadline_cause, begin, graph::EdgeKind::DataDependency });
    runtime.submit(std::move(mutation));
}

bool Storage::accept(std::size_t request, graph::Time now_ns) {
    auto & result = results_[request];
    auto & operation = prefetches_.at(request);
    result.ready = true;
    result.accepted_ns = now_ns;
    result.accepted_pages = result.published_pages;
    operation.prefix.insert(operation.prefix.end(), operation.keys.begin(), operation.keys.begin() + result.accepted_pages);
    result.inserted_pages = operation.prefix.size() - cache_.match(operation.prefix).pages;
    cache_.commit_host(operation.prefix, operation.buffer);
    reserved_pages_ -= result.accepted_pages;
    cache_.trim(operation.buffer, result.worker_done ? 0 : operation.keys.size() - result.accepted_pages);
    occupied_pages_ -= result.requested_pages;
    cache_.unpin_host(operation.anchor);
    if (result.worker_done) {
        cache_.release(operation.buffer);
        prefetches_.erase(request);
    }
    return true;
}

bool Storage::finish_read(ProgramRuntime & runtime, std::size_t request, std::size_t rank, graph::NodeId cause) {
    auto & worker = workers_[rank];
    worker.read_completion = cause;
    worker.reads.pop_front();
    auto & operation = prefetches_.at(request);
    operation.completed.push_back(cause);
    const auto terminated = operation.workers[rank].terminated;
    const auto done = operation.completed.size() == participants_;
    if (done) {
        completion_frontier_[0] = join(runtime, operation.completed);
        auto & result = results_[request];
        result.worker_done = true;
        result.worker_finished_ns = runtime.now_ns();
        if (result.ready) {
            reserved_pages_ -= operation.keys.size() - result.accepted_pages;
            cache_.release(operation.buffer);
            prefetches_.erase(request);
        }
        else {
            reserved_pages_ -= operation.keys.size() - result.published_pages;
            cache_.trim(operation.buffer, result.published_pages);
        }
    }
    if (!worker.reads.empty()) start_read(runtime, worker.reads.front(), rank, cause);
    return done || terminated;
}

void Storage::enqueue_write(ProgramRuntime & runtime, RadixId source, std::vector<PrefixId> pages, graph::NodeId cause) {
    cache_.pin_host(source);
    const auto id = stats_.writes++;
    writes_.emplace(id, Write{ source, std::move(pages), cause, {} });
    for (std::size_t rank = 0; rank < participants_; ++rank) {
        auto & worker = workers_[rank];
        worker.writes.push_back({ id });
        if (worker.writes.size() == 1) start_write(runtime, rank, worker.write_completion);
    }
}

void Storage::start_write(ProgramRuntime & runtime, std::size_t rank, std::optional<graph::NodeId> cause) {
    auto & cursor = workers_[rank].writes.front();
    const auto & operation = writes_.at(cursor.operation);
    cursor.batch_end = std::min(cursor.index + backend_.batch_pages, operation.pages.size());
    service(runtime,
            backend_.gather,
            cursor.batch_end - cursor.index,
            rank,
            true,
            action(ActionKind::Gathered, rank),
            queued_cause(runtime, operation.enqueue_cause, cause));
}

void Storage::reserve_file(std::size_t rank) {
    auto & files = workers_[rank].files;
    if (config_.capacity_pages && files.size() >= *config_.capacity_pages) {
        const auto oldest = std::ranges::min_element(files, {}, [](const auto & item) { return item.second; });
        files.erase(oldest);
        ++stats_.rank_disk_evictions;
    }
}

bool Storage::write_page(ProgramRuntime & runtime, std::size_t rank, graph::NodeId cause) {
    auto & worker = workers_[rank];
    auto & cursor = worker.writes.front();
    auto & operation = writes_.at(cursor.operation);
    if (cursor.index < cursor.batch_end) {
        const auto key = operation.pages[cursor.index];
        if (worker.files.contains(key)) service(runtime, backend_.existing_page, 1, rank, true, action(ActionKind::PageTouched, rank), cause);
        else {
            reserve_file(rank);
            service(runtime, backend_.new_page, 1, rank, true, action(ActionKind::PageWritten, rank), cause);
        }
        return false;
    }
    if (cursor.index != operation.pages.size()) {
        start_write(runtime, rank, cause);
        return false;
    }
    worker.write_completion = cause;
    operation.completed.push_back(cause);
    const auto done = operation.completed.size() == participants_;
    if (done) {
        completion_frontier_[1] = join(runtime, operation.completed);
        cache_.unpin_host(operation.source);
        writes_.erase(cursor.operation);
    }
    worker.writes.pop_front();
    if (!worker.writes.empty()) start_write(runtime, rank, cause);
    return done;
}

bool Storage::on_event(ProgramRuntime & runtime, const simulation::Event & event) {
    const auto subject = event.action.subject;
    const auto request = subject / participants_, rank = subject % participants_;
    switch (static_cast<ActionKind>(event.action.kind)) {
    case ActionKind::Allocated:
        read_page(runtime, request, rank, event.identity);
        break;
    case ActionKind::PageRead: {
        auto & operation = prefetches_.at(request);
        auto & cursor = operation.workers[rank];
        touch(rank, operation.keys[cursor.read_index++]);
        ++stats_.rank_read_pages;
        read_page(runtime, request, rank, event.identity);
        break;
    }
    case ActionKind::PageCopied: {
        auto & operation = prefetches_.at(request);
        auto & cursor = operation.workers[rank];
        ++stats_.rank_copied_pages;
        if (cursor.terminated) {
            ++stats_.rank_rejected_copies;
            return_read(runtime, request, rank, event.identity);
        }
        else {
            cursor.publications.push_back(event.identity);
            ++cursor.copy_index;
            auto & result = results_[request];
            const auto slowest = std::ranges::min_element(operation.workers, {}, [](const ReadCursor & worker) { return worker.publications.size(); });
            const auto common = slowest->publications.size();
            if (common > result.published_pages) {
                // Only the common prefix is visible; workers never wait here.
                std::vector<graph::NodeId> publications;
                for (const auto & participant : operation.workers) publications.push_back(participant.publications[common - 1]);
                operation.publication_cause = join(runtime, publications);
                result.published_pages = common;
            }
            copy_page(runtime, request, rank, event.identity);
            return result.published_pages == operation.keys.size();
        }
        break;
    }
    case ActionKind::ReadReturned: {
        auto & operation = prefetches_.at(request);
        auto & cursor = operation.workers[rank];
        if (cursor.terminated || cursor.batch_end == operation.keys.size()) return finish_read(runtime, request, rank, event.identity);
        cursor.batch_begin = cursor.batch_end;
        start_read(runtime, request, rank, event.identity);
        break;
    }
    case ActionKind::Accept:
        completion_frontier_[2] = event.identity;
        return accept(subject, runtime.now_ns());
    case ActionKind::Timeout: {
        const auto cause = runtime.next_node();
        runtime.submit({ owner, { { 0, Boundary{} } }, {} });
        prefetches_.at(subject).deadline_cause = cause;
        return results_[subject].first_check_ns.has_value();
    }
    case ActionKind::Gathered: {
        const auto & cursor = workers_[subject].writes.front();
        service(runtime, backend_.batch_prepare, cursor.batch_end - cursor.index, subject, true, action(ActionKind::Prepared, subject), event.identity);
        break;
    }
    case ActionKind::Prepared:
        return write_page(runtime, subject, event.identity);
    case ActionKind::PageWritten: {
        auto & worker = workers_[subject];
        auto & cursor = worker.writes.front();
        const auto & operation = writes_.at(cursor.operation);
        worker.files[operation.pages[cursor.index++]] = ++access_;
        ++stats_.rank_new_pages;
        stats_.peak_rank_stored_pages = std::max(stats_.peak_rank_stored_pages, worker.files.size());
        return write_page(runtime, subject, event.identity);
    }
    case ActionKind::PageTouched: {
        auto & cursor = workers_[subject].writes.front();
        const auto & operation = writes_.at(cursor.operation);
        touch(subject, operation.pages[cursor.index++]);
        ++stats_.rank_existing_pages;
        return write_page(runtime, subject, event.identity);
    }
    }
    return false;
}

} // namespace markov::trace_graph::planning
