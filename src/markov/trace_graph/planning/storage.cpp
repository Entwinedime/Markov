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
      results_(requests) {}

bool Storage::service(ProgramRuntime & runtime, const WorkKind & kind, std::size_t pages, bool writer, simulation::Action action,
                      std::optional<graph::NodeId> cause) {
    const Work work{
        kind,
        { { "bytes", static_cast<double>(pages * page_bytes_) },
          { "pages", static_cast<double>(pages) },
          { "operations", 1 },
          { "page_bytes", static_cast<double>(page_bytes_) } }
    };
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ owner, {}, {} };
    for (std::size_t rank = 0; rank < participants_; ++rank) {
        mutation.operations.push_back({ resource_begin_ + 2 * rank + writer, std::cref(work) });
        if (cause) mutation.dependencies.push_back({ *cause, begin + rank, graph::EdgeKind::DataDependency });
        mutation.dependencies.push_back({ begin + rank, begin + participants_, graph::EdgeKind::DataDependency });
    }
    mutation.operations.push_back({ 0, Boundary{}, action });
    return runtime.submit(std::move(mutation)).has_value();
}

bool Storage::prepare(ProgramRuntime & runtime, std::size_t request, std::span<const PrefixId> lookup, const CacheMatch & matched,
                      std::optional<graph::NodeId> cause) {
    auto & result = results_[request];
    if (result.examined) return result.ready;
    result.examined = true;
    const auto host_pages = cache_.page_count(matched.host);
    const auto start = std::max(host_pages, cache_.page_count(matched.device));
    const auto requested = lookup.size() - start;
    if (requested * page_tokens_ < config_.threshold_tokens
        || occupied_pages_ * page_tokens_ >= static_cast<std::size_t>(config_.prefetch_limit_pages * page_tokens_)) {
        result.ready = result.worker_done = true;
        return true;
    }
    cache_.pin_host(matched.host);
    stats_.host_evictions += cache_.reclaim_host(requested);
    const auto count = std::min(requested, cache_.available_pages(Tier::Host));
    if (count * page_tokens_ < config_.threshold_tokens) {
        cache_.unpin_host(matched.host);
        result.ready = result.worker_done = true;
        return true;
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
        if (!next || !files_.contains(*next)) break;
        keys.push_back(*next);
        anchor = *next;
    }
    result.queried_pages = keys.size();
    if (keys.size() * page_tokens_ < config_.threshold_tokens) {
        cache_.release(buffer);
        cache_.unpin_host(matched.host);
        result.ready = result.worker_done = true;
        return true;
    }
    cache_.trim(buffer, keys.size());
    Prefetch operation{};
    operation.prefix.assign(lookup.begin(), lookup.begin() + host_pages);
    operation.keys = std::move(keys);
    operation.buffer = buffer;
    operation.anchor = matched.host;
    operation.enqueue_cause = cause;
    reserved_pages_ += operation.keys.size();
    occupied_pages_ += count;
    prefetches_.emplace(request, std::move(operation));
    read_queue_.push_back(request);
    ++stats_.prefetches;
    if (read_queue_.size() == 1) start_read(runtime, request, cause);

    if (config_.policy == PrefetchPolicy::BestEffort) stop(runtime, request, cause);
    else if (config_.policy == PrefetchPolicy::Timeout) {
        const auto budget = std::min<long double>(
            config_.timeout_max_ns,
            config_.timeout_base_ns + static_cast<long double>(config_.timeout_per_ki_token_ns) * result.queried_pages * page_tokens_ / 1'024);
        if (budget >= static_cast<long double>(std::numeric_limits<graph::Time>::max()) - runtime.now_ns())
            throw std::runtime_error("Prefetch deadline exceeds the execution clock range");

        // The native condition is elapsed > budget, hence the next integer ns.
        prefetches_.at(request).timer =
            runtime.schedule_at(runtime.now_ns() + static_cast<graph::Time>(std::floor(budget)) + 1, owner, action(ActionKind::Timeout, request));
    }
    return false;
}

void Storage::start_read(ProgramRuntime & runtime, std::size_t request, std::optional<graph::NodeId> cause) {
    auto & operation = prefetches_.at(request);
    operation.batch_end = std::min(operation.batch_begin + backend_.batch_pages, operation.keys.size());
    operation.read_index = operation.copy_index = operation.batch_begin;
    operation.readable.assign(operation.batch_end - operation.batch_begin, false);
    service(runtime,
            backend_.allocate,
            operation.batch_end - operation.batch_begin,
            false,
            action(ActionKind::Allocated, request),
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

void Storage::touch(PrefixId key) {
    const auto found = files_.find(key);
    if (found != files_.end()) found->second = ++access_;
}

void Storage::read_page(ProgramRuntime & runtime, std::size_t request, graph::NodeId cause) {
    auto & operation = prefetches_.at(request);
    while (operation.read_index < operation.batch_end) {
        const auto page = operation.read_index;
        if (files_.contains(operation.keys[page])) {
            operation.readable[page - operation.batch_begin] = true;
            service(runtime, backend_.read_page, 1, false, action(ActionKind::PageRead, request), cause);
            return;
        }
        // A missing file has no payload transfer. Metadata CPU work remains
        // outside this component, rather than being charged as a full read.
        ++operation.read_index;
    }
    copy_page(runtime, request, cause);
}

void Storage::copy_page(ProgramRuntime & runtime, std::size_t request, graph::NodeId cause) {
    auto & operation = prefetches_.at(request);
    if (operation.copy_index == operation.batch_end || !operation.readable[operation.copy_index - operation.batch_begin]) {
        if (operation.copy_index != operation.batch_end) operation.terminated = true;
        return_read(runtime, request, cause);
        return;
    }
    // Even after termination the native loop copies one page before increment
    // rejects publication; an in-flight operation cannot disappear at timeout.
    service(runtime, backend_.copy_publish, 1, false, action(ActionKind::PageCopied, request), cause);
}

void Storage::return_read(ProgramRuntime & runtime, std::size_t request, graph::NodeId cause) {
    const auto & operation = prefetches_.at(request);
    service(runtime, backend_.read_return, operation.batch_end - operation.batch_begin, false, action(ActionKind::ReadReturned, request), cause);
}

void Storage::stop(ProgramRuntime & runtime, std::size_t request, std::optional<graph::NodeId> cause) {
    auto & operation = prefetches_.at(request);
    if (operation.stopping) return;
    operation.stopping = operation.terminated = true;
    if (operation.timer) runtime.cancel_timer(*operation.timer);
    auto & result = results_[request];
    result.stopped_early = result.published_pages < operation.keys.size();
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ owner, { { 0, Boundary{}, action(ActionKind::Accept, request) } }, {} };
    if (cause) mutation.dependencies.push_back({ *cause, begin, graph::EdgeKind::DataDependency });
    if (operation.enqueue_cause) mutation.dependencies.push_back({ *operation.enqueue_cause, begin, graph::EdgeKind::DataDependency });
    runtime.submit(std::move(mutation));
}

bool Storage::accept(std::size_t request) {
    auto & result = results_[request];
    auto & operation = prefetches_.at(request);
    result.ready = true;
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

bool Storage::finish_read(ProgramRuntime & runtime, std::size_t request, graph::NodeId cause) {
    completion_frontier_[0] = cause;
    auto & result = results_[request];
    auto & operation = prefetches_.at(request);
    result.worker_done = true;
    if (result.ready) {
        reserved_pages_ -= operation.keys.size() - result.accepted_pages;
        cache_.release(operation.buffer);
        prefetches_.erase(request);
    }
    else {
        reserved_pages_ -= operation.keys.size() - result.published_pages;
        cache_.trim(operation.buffer, result.published_pages);
        stop(runtime, request, cause);
    }
    read_queue_.pop_front();
    if (!read_queue_.empty()) start_read(runtime, read_queue_.front(), cause);
    return true;
}

void Storage::enqueue_write(ProgramRuntime & runtime, RadixId source, std::vector<PrefixId> pages, graph::NodeId cause) {
    cache_.pin_host(source);
    write_queue_.push_back({ source, std::move(pages), 0, 0, false, cause });
    ++stats_.writes;
    if (write_queue_.size() == 1) start_write(runtime, cause);
}

void Storage::start_write(ProgramRuntime & runtime, std::optional<graph::NodeId> cause) {
    auto & operation = write_queue_.front();
    operation.batch_end = std::min(operation.index + backend_.batch_pages, operation.pages.size());
    operation.prepared = false;
    service(runtime,
            backend_.gather_check,
            operation.batch_end - operation.index,
            true,
            action(ActionKind::Gathered, 0),
            queued_cause(runtime, operation.enqueue_cause, cause));
}

void Storage::reserve_file() {
    if (config_.capacity_pages && files_.size() >= *config_.capacity_pages) {
        const auto oldest = std::ranges::min_element(files_, {}, [](const auto & item) { return item.second; });
        files_.erase(oldest);
        ++stats_.disk_evictions;
    }
}

bool Storage::write_page(ProgramRuntime & runtime, graph::NodeId cause) {
    auto & operation = write_queue_.front();
    while (operation.index < operation.batch_end) {
        const auto key = operation.pages[operation.index];
        if (files_.contains(key)) {
            touch(key);
            ++stats_.existing_pages;
            ++operation.index;
            continue;
        }
        if (!operation.prepared) {
            operation.prepared = true;
            service(runtime, backend_.new_prepare, operation.batch_end - operation.index, true, action(ActionKind::Prepared, 0), cause);
            return false;
        }
        reserve_file();
        service(runtime, backend_.new_page, 1, true, action(ActionKind::PageWritten, 0), cause);
        return false;
    }
    if (operation.index != operation.pages.size()) {
        start_write(runtime, cause);
        return false;
    }
    cache_.unpin_host(operation.source);
    completion_frontier_[1] = cause;
    write_queue_.pop_front();
    if (!write_queue_.empty()) start_write(runtime, cause);
    return true;
}

bool Storage::on_event(ProgramRuntime & runtime, const simulation::Event & event) {
    const auto request = event.action.subject;
    switch (static_cast<ActionKind>(event.action.kind)) {
    case ActionKind::Allocated:
        read_page(runtime, request, event.identity);
        break;
    case ActionKind::PageRead: {
        auto & operation = prefetches_.at(request);
        touch(operation.keys[operation.read_index++]);
        ++stats_.read_pages;
        read_page(runtime, request, event.identity);
        break;
    }
    case ActionKind::PageCopied: {
        auto & operation = prefetches_.at(request);
        ++stats_.copied_pages;
        if (operation.terminated) {
            ++stats_.rejected_copies;
            return_read(runtime, request, event.identity);
        }
        else {
            ++results_[request].published_pages;
            ++operation.copy_index;
            if (results_[request].published_pages == operation.keys.size()) stop(runtime, request, event.identity);
            copy_page(runtime, request, event.identity);
        }
        break;
    }
    case ActionKind::ReadReturned: {
        auto & operation = prefetches_.at(request);
        if (operation.terminated || operation.batch_end == operation.keys.size()) return finish_read(runtime, request, event.identity);
        operation.batch_begin = operation.batch_end;
        start_read(runtime, request, event.identity);
        break;
    }
    case ActionKind::Accept:
        completion_frontier_[2] = event.identity;
        return accept(request);
    case ActionKind::Timeout:
        stop(runtime, request, std::nullopt);
        break;
    case ActionKind::Gathered:
    case ActionKind::Prepared:
        return write_page(runtime, event.identity);
    case ActionKind::PageWritten: {
        auto & operation = write_queue_.front();
        files_[operation.pages[operation.index++]] = ++access_;
        ++stats_.new_pages;
        stats_.peak_stored_pages = std::max(stats_.peak_stored_pages, files_.size());
        return write_page(runtime, event.identity);
    }
    }
    return false;
}

} // namespace markov::trace_graph::planning
