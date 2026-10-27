#include "markov/trace_graph/modules/hicache/model/prefetch_execution.hpp"

#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {
namespace {
core::DagSyntheticNodeSpec task(const HiCachePrefetchOperation & op, std::string name, uint64_t duration = 0) {
    return {
        .name = std::move(name),
        .category = "hicache_patch",
        .lane_key = op.io_schedule.resource_lane,
        .duration = duration,
        .attrs = { { "operation_id", op.header.operation_id },
                  { "cache_scope", op.header.cache_scope },
                  { "request_id", op.header.request_id },
                  { "source_fact_id", std::to_string(op.header.source_node_id) } }
    };
}
} // namespace

void HiCachePrefetchExecution::enqueue(const HiCacheFact & candidate, simulation::FutureDag & future) {
    const auto * op = state_.prefetch_candidate_operation(candidate);
    if (!op || !op->payload_transfer_issued) return;
    if (submitted_.contains(op)) throw std::logic_error("Prefetch payload already submitted to execution");
    const auto index = executions_.size();
    const auto begin = future.append(task(*op, "hicache_prefetch_worker_start"));
    const auto complete = future.append(task(*op, "hicache_prefetch_worker_return"));
    future.depend(begin, complete);
    if (const auto tail = lane_tail_.find(op->io_schedule.resource_lane); tail != lane_tail_.end()) future.depend(tail->second, begin);
    lane_tail_[op->io_schedule.resource_lane] = complete;
    submitted_.insert(op);
    executions_.push_back({ op, complete, op->hit_pages.size(), {} });
    events_.emplace(begin, Event{ index, Step::Begin });
    events_.emplace(complete, Event{ index, Step::Complete });
}

void HiCachePrefetchExecution::schedule(size_t index, Step step, uint64_t duration, simulation::FutureDag & future) {
    const auto & run = executions_.at(index);
    const auto work = future.append(task(*run.operation, step == Step::Publication ? "hicache_prefetch_read_copy" : "hicache_prefetch_cleanup", duration));
    const auto boundary = future.append(task(*run.operation, step == Step::Publication ? "hicache_prefetch_increment" : "hicache_prefetch_batch_return"),
                                        step == Step::Publication ? simulation::BoundaryOrder::AfterConcurrentStarts : simulation::BoundaryOrder::Ordinary);
    future.depend(work, boundary);
    future.depend(boundary, run.completion);
    events_.emplace(boundary, Event{ index, step });
}

void HiCachePrefetchExecution::begin_batch(size_t index, uint64_t time, simulation::FutureDag & future) {
    auto & run = executions_.at(index);
    const auto pages = std::min(state_.storage_batch_pages(), run.remaining_pages);
    run.batch = completion_.prefetch_batch(state_.page_bytes(), pages, time);
    run.publication = 0;
    run.remaining_pages -= pages;
    run.executed.push_back({ .page_count = pages, .start_ts = time, .copied_page_count = 0 });
    state_.record_prefetch_batch(run.operation->header, run.executed.size() - 1, run.executed.back());
    // A worker enters the first batch even if already stopped. Cancellation is
    // tested after its read and first copy, never at worker start.
    schedule(index, Step::Publication, run.batch.publications.front().at_us - time, future);
}

void HiCachePrefetchExecution::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    const auto found = events_.find(node);
    if (found == events_.end()) return;
    const auto [index, step] = found->second;
    events_.erase(found);
    auto & run = executions_.at(index);
    const auto & op = *run.operation;
    if (step == Step::Begin) {
        state_.begin_prefetch_io(op.header, time);
        begin_batch(index, time, future);
    }
    else if (step == Step::Publication) {
        auto & batch = run.executed.back();
        const auto & publication = run.batch.publications.at(run.publication++);
        *batch.copied_page_count += publication.page_count;
        run.cancelled = op.execution_stop_ts && time >= *op.execution_stop_ts;
        if (!run.cancelled) batch.page_ready_ts.push_back(time);
        state_.record_prefetch_batch(op.header, run.executed.size() - 1, batch);
        if (!run.cancelled && run.publication < run.batch.publications.size()) {
            // The service model's coordinates describe relative work segments,
            // not a second execution deadline. Final cost transforms can change
            // elapsed time; keep spacing between modeled publication points.
            schedule(index, Step::Publication, run.batch.publications[run.publication].at_us - publication.at_us, future);
        }
        else {
            const auto returned = run.cancelled ? publication.cancelled_return_us.value() : run.batch.ready_ts;
            schedule(index, Step::BatchReturn, returned - publication.at_us, future);
        }
    }
    else if (step == Step::BatchReturn) {
        run.executed.back().ready_ts = time;
        state_.record_prefetch_batch(op.header, run.executed.size() - 1, run.executed.back());
        if (!run.cancelled && run.remaining_pages) begin_batch(index, time, future);
    }
    else { state_.complete_prefetch_io(op.header, time); }
}

} // namespace markov::trace_graph::modules::hicache::model
