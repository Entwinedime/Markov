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

std::optional<size_t> HiCachePrefetchExecution::enqueue(const HiCacheFact & candidate, simulation::FutureDag & future) {
    const auto * op = state_.prefetch_candidate_operation(candidate);
    if (!op || !op->payload_transfer_issued) return std::nullopt;
    if (submitted_.contains(op)) throw std::logic_error("Prefetch payload already submitted to execution");
    const auto index = executions_.size();
    const auto begin = future.append(task(*op, "hicache_prefetch_worker_start"));
    const auto complete = future.append(task(*op, "hicache_prefetch_worker_return"));
    future.depend(begin, complete);
    if (const auto tail = lane_tail_.find(op->io_schedule.resource_lane); tail != lane_tail_.end()) future.depend(tail->second, begin);
    lane_tail_[op->io_schedule.resource_lane] = complete;
    submitted_.emplace(op, complete);
    executions_.push_back({ op, complete, op->hit_pages.size(), {} });
    events_.emplace(begin, Event{ index, Step::Begin });
    events_.emplace(complete, Event{ index, Step::Complete });
    return complete;
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
    const auto pages = std::min(config_.io_cost.storage_batch_pages, run.remaining_pages);
    const auto & service = require_hicache_service(config_.io_cost, "prefetch", config_.kv_bytes_per_page, pages);
    const auto batch = hicache_prefetch_batch(service, config_.kv_bytes_per_page, pages, time);
    if (!batch) throw std::logic_error("Prefetch execution requires measured read/copy/return stages");
    run.batch = *batch;
    run.publication = 0;
    run.remaining_pages -= pages;
    run.executed.push_back({ .page_count = pages, .start_ts = time, .copied_page_count = 0 });
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
        if (time != op.io_schedule.start_ts) throw std::logic_error("Prefetch worker start differs from target resource queue");
        begin_batch(index, time, future);
    }
    else if (step == Step::Publication) {
        auto & batch = run.executed.back();
        const auto & publication = run.batch.publications.at(run.publication++);
        *batch.copied_page_count += publication.page_count;
        run.cancelled = op.execution_stop_ts && time >= *op.execution_stop_ts;
        if (!run.cancelled) batch.page_ready_ts.push_back(time);
        if (!run.cancelled && run.publication < run.batch.publications.size()) {
            schedule(index, Step::Publication, run.batch.publications[run.publication].at_us - time, future);
        }
        else {
            const auto returned = run.cancelled ? publication.cancelled_return_us.value() : run.batch.ready_ts;
            schedule(index, Step::BatchReturn, returned - time, future);
        }
    }
    else if (step == Step::BatchReturn) {
        run.executed.back().ready_ts = time;
        if (!run.cancelled && run.remaining_pages) begin_batch(index, time, future);
    }
    else {
        // One physical clock, not an independently adjusted cost or completion.
        const auto & planned = op.io_schedule.batches;
        if (time != op.io_schedule.ready_ts || planned.size() != run.executed.size())
            throw std::logic_error("Executed prefetch return differs from state service plan");
        for (size_t i = 0; i < planned.size(); ++i) {
            const auto & actual = run.executed[i];
            const auto & expected = planned[i];
            if (actual.start_ts != expected.start_ts || actual.ready_ts != expected.ready_ts || actual.page_count != expected.page_count
                || actual.page_ready_ts != expected.page_ready_ts || actual.copied_page_count != expected.copied_page_count)
                throw std::logic_error("Executed prefetch pages differ from state service plan");
        }
        state_.complete_prefetch_io(op.header, time);
    }
}

} // namespace markov::trace_graph::modules::hicache::model
