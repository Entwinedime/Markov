#include "markov/trace_graph/planning/storage.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace markov::trace_graph::planning {

void Storage::start_query(ProgramRuntime & runtime, std::size_t request) {
    auto & operation = prefetches_.at(request);
    const auto requested = results_[request].requested_pages;
    auto common_hits = operation.keys.size();
    std::size_t hashed_pages = 0, checked_pages = 0;
    for (const auto & worker : workers_) {
        std::size_t hits = 0;
        while (hits < operation.keys.size() && worker.files.contains(operation.keys[hits])) ++hits;
        common_hits = std::min(common_hits, hits);
        hashed_pages = std::max(hashed_pages, hits == requested ? requested : std::min(requested, (hits / backend_.batch_pages + 1) * backend_.batch_pages));
        checked_pages = std::max(checked_pages, std::min(requested, hits + 1));
    }
    operation.query_hits = common_hits;

    // One native query thread per rank participates in the same MIN reduction.
    // shortcut: snapshot files at query start; qualify concurrent mutations separately.
    const Work work{
        backend_.query,
        { { "hashed_pages", static_cast<double>(hashed_pages) },
                { "checked_pages", static_cast<double>(checked_pages) },
                { "page_tokens", static_cast<double>(page_tokens_) },
                { "participants", static_cast<double>(participants_) } }
    };
    const auto cause = queued_cause(runtime, operation.enqueue_cause, completion_frontier_[3]);
    const auto node = runtime.next_node();
    PlannedMutation mutation{ owner, { { resource_begin_ + 2 * participants_, std::cref(work), action(ActionKind::Queried, request) } }, {} };
    if (cause) mutation.dependencies.push_back({ *cause, node, graph::EdgeKind::Submission });
    if (runtime.records_graph()) mutation.operations.front().position = OperationPosition{ request, 0, 0, "storage_query" };
    runtime.submit(std::move(mutation));
}

void Storage::trim_buffer(std::size_t request, std::size_t remaining_pages) {
    const auto buffer = prefetches_.at(request).buffer;
    reserved_pages_ -= cache_.buffer_pages(buffer) - remaining_pages;
    cache_.trim(buffer, remaining_pages);
}

bool Storage::finish_query(ProgramRuntime & runtime, std::size_t request, graph::NodeId cause) {
    auto & operation = prefetches_.at(request);
    auto & result = results_[request];
    operation.queried = true;
    operation.query_cause = cause;
    result.queried_pages = operation.query_hits;
    result.query_finished_ns = runtime.now_ns();
    const auto readable = operation.query_hits * page_tokens_ >= config_.threshold_tokens;
    operation.keys.resize(readable ? operation.query_hits : 0);
    if (readable) {
        ++stats_.prefetches;
        operation.enqueue_cause = cause;
        for (std::size_t rank = 0; rank < participants_; ++rank) {
            auto & worker = workers_[rank];
            worker.reads.push_back(request);
            if (worker.reads.size() == 1) start_read(runtime, request, rank, worker.read_completion);
        }
    }
    else {
        result.worker_done = true;
        result.worker_finished_ns = runtime.now_ns();
        if (result.ready) {
            trim_buffer(request, 0);
            cache_.release(operation.buffer);
            prefetches_.erase(request);
        }
    }
    completion_frontier_[3] = cause;
    queries_.pop_front();
    if (!queries_.empty()) start_query(runtime, queries_.front());
    return true;
}

} // namespace markov::trace_graph::planning
