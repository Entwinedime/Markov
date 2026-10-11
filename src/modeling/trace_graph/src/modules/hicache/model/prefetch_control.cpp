#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {
namespace {
uint64_t locally_published_pages(const HiCachePrefetchOperation & op) {
    uint64_t pages = 0;
    for (const auto & batch : op.io_schedule.batches) pages += batch.page_ready_ts.size();
    return pages;
}
} // namespace

const HiCachePrefetchOperation * HiCacheState::prefetch_operation(const HiCacheFact & fact) const {
    const auto scope = scopes_.find(normalized_scope(fact));
    return scope == scopes_.end() ? nullptr : scope->second.async_ops.prefetch_for_request(scoped_request_key(fact));
}

const HiCachePrefetchOperation * HiCacheState::prefetch_candidate_operation(const HiCacheFact & fact) const {
    const auto scope = scopes_.find(normalized_scope(fact));
    if (scope == scopes_.end()) return nullptr;
    const auto request = scoped_request_key(fact);
    for (const auto & [id, op] : scope->second.async_ops.prefetch_ops())
        if (op.header.source_node_id == fact.source_node_id && op.header.request_key == request) return &op;
    return nullptr;
}

void HiCacheState::queue_prefetch_host_release(ScopedState & scope, const HiCachePrefetchOperation & op, uint64_t pages) {
    if (pages) scope.prefetch_host_release_queue.emplace_back(op.header.operation_id, pages);
}

uint64_t HiCacheState::query_prefetch_storage(const HiCacheFact & fact) {
    auto & scope = scope_state(fact);
    const auto * candidate = prefetch_candidate_operation(fact);
    auto * op = candidate ? &scope.async_ops.prefetch_ops().at(candidate->header.operation_id) : nullptr;
    if (!execution_prefetch_control_ || !op || op->query_sample_ts || fact.ts < op->header.enqueue_ts)
        throw std::logic_error("Prefetch query requires a new local storage sample at a causal time");
    op->hit_pages = scope.storage.contiguous_readable_prefix(op->planned_pages);
    op->query_sample_ts = fact.ts;
    return op->hit_pages.size();
}

void HiCacheState::complete_prefetch_query(const HiCacheFact & fact, uint64_t common_hit_pages) {
    auto & scope = scope_state(fact);
    const auto * candidate = prefetch_candidate_operation(fact);
    auto * op = candidate ? &scope.async_ops.prefetch_ops().at(candidate->header.operation_id) : nullptr;
    if (!execution_prefetch_control_ || !op || !op->query_sample_ts || op->query_return_ts || fact.ts < *op->query_sample_ts
        || common_hit_pages > op->hit_pages.size())
        throw std::logic_error("Prefetch query completion requires a causal MIN result within the local hit prefix");
    op->query_return_ts = fact.ts;
    op->hit_pages.resize(common_hit_pages);
    op->host_insert_pages.resize(op->host_visible_offset_pages);
    op->host_insert_pages.insert(op->host_insert_pages.end(), op->hit_pages.begin(), op->hit_pages.end());
    if (op->hit_pages.size() < policy_.prefetch_threshold_pages()) {
        scope.prefetch_revoke_queue.push_back(op->header.operation_id);
        queue_prefetch_host_release(scope, *op, op->requested_host_pages);
        return;
    }
    queue_prefetch_host_release(scope, *op, op->requested_host_pages - op->hit_pages.size());
    op->payload_transfer_issued = true;
    op->io_schedule = schedule_target_io(normalized_scope(fact), "prefetch", fact.ts, op->hit_pages.size(), {}, op->execution_stop_ts);
    op->service_pages = prefix_to(op->hit_pages, op->io_schedule.effective_byte_count / config_.kv_bytes_per_page);
}

HiCachePrefetchCheck HiCacheState::sample_prefetch_check(const HiCacheFact & fact) const {
    if (!execution_prefetch_control_) throw std::logic_error("Prefetch checks require execution-driven state");
    const auto * op = prefetch_operation(fact);
    if (!op || !prefetch_active(*op)) return {};
    if (fact.ts < op->header.enqueue_ts) throw std::logic_error("Prefetch check precedes its candidate");
    if (policy_.prefetch_policy() == "best_effort") return { .ongoing = true, .can_stop = true, .terminated = op->execution_stop_ts.has_value() };
    // hash_value is assigned only after hit MIN accepts the query. A local
    // storage sample is not yet the operation's published hash list.
    const auto hash_pages = op->payload_transfer_issued ? op->hit_pages.size() : 0;
    uint64_t published = 0;
    for (const auto & batch : op->io_schedule.batches)
        for (const auto at : batch.page_ready_ts) published += at < fact.ts;
    const auto deadline = policy_.prefetch_timeout_deadline_ts(
        { .enqueue_ts = op->header.enqueue_ts,
          .token_count = core::checked_multiply_u64(hash_pages, config_.page_size, "Prefetch check token count overflow") });
    return { .ongoing = true,
             .can_stop = (hash_pages && published == hash_pages) || (policy_.prefetch_policy() == "timeout" && deadline && fact.ts > *deadline),
             .terminated = op->execution_stop_ts.has_value() };
}

uint64_t HiCacheState::stop_prefetch(const HiCacheFact & fact) {
    auto & scope = scope_state(fact);
    auto * op = scope.async_ops.prefetch_for_request(scoped_request_key(fact));
    if (!execution_prefetch_control_ || !op) throw std::logic_error("Prefetch stop requires an execution-driven operation");
    if (op->execution_stop_ts) {
        if (fact.ts < *op->execution_stop_ts) throw std::logic_error("Prefetch stop cannot precede an earlier termination");
        return locally_published_pages(*op); // mark_terminate is idempotent after an abort or peer cancellation.
    }
    if (fact.ts < op->header.enqueue_ts) throw std::logic_error("Prefetch stop cannot precede enqueue");
    op->execution_stop_ts = fact.ts;
    op->policy_stop_ts = fact.ts;
    if (!op->payload_transfer_issued) return 0; // Query may still issue its first batch later.

    // The worker observes termination after its next copy. Its DAG return
    // releases the lane; neither this stop nor state recomputes queued costs.
    return locally_published_pages(*op);
}

void HiCacheState::publish_prefetch(const HiCacheFact & fact, uint64_t visible_pages) {
    auto & scope = scope_state(fact);
    auto * op = scope.async_ops.prefetch_for_request(scoped_request_key(fact));
    if (!execution_prefetch_control_ || !op || !op->execution_stop_ts || !prefetch_active(*op))
        throw std::logic_error("Prefetch visibility requires a stopped, unpublished execution-driven operation");
    if (fact.ts < *op->execution_stop_ts || visible_pages > locally_published_pages(*op))
        throw std::logic_error("Prefetch visibility precedes stop or exceeds local publication");
    bind_prefetch_consumer_boundary(fact, scope, *op);
    op->header.boundary_ts = op->target_boundary_ts = fact.ts;
    op->header.boundary_epoch = op->header.consumer_epoch;
    op->completed_pages = prefix_to(op->hit_pages, visible_pages);
    op->completed_byte_count = core::checked_multiply_u64(visible_pages, config_.kv_bytes_per_page, "Prefetch visible bytes overflow");
    const auto deadline = policy_.prefetch_timeout_deadline_ts(
        { .enqueue_ts = op->header.enqueue_ts,
          .token_count =
              core::checked_multiply_u64(op->payload_transfer_issued ? op->hit_pages.size() : 0, config_.page_size, "Prefetch timeout token count overflow") });
    op->timeout_deadline_ts = deadline.value_or(0);
    const bool completed = op->payload_transfer_issued && !op->hit_pages.empty() && visible_pages == op->hit_pages.size();
    op->timed_out = policy_.prefetch_policy() == "timeout" && deadline && *op->execution_stop_ts > *deadline && !completed;
    // The executed control path pays MAX/MIN and retries. Do not add the old
    // enqueue-to-deadline proxy again or wait for unused background cleanup.
    op->policy_wait_duration_us = 0;
    op->service_consumer_dependency_required = false;
    op->visibility_dependency_required = visible_pages != 0;
    apply_prefetch_ready(fact, scope, *op);
    queue_prefetch_host_release(scope, *op, locally_published_pages(*op) - visible_pages);
}

void HiCacheState::complete_prefetch_io(const HiCacheFact & fact) {
    const auto * op = prefetch_operation(fact);
    if (!op) throw std::logic_error("Prefetch return has no operation");
    complete_prefetch_io(op->header, fact.ts);
}

void HiCacheState::begin_prefetch_io(const HiCacheOperationHeader & operation, uint64_t timestamp_us) {
    auto & op = scopes_.at(operation.cache_scope).async_ops.prefetch_ops().at(operation.operation_id);
    op.io_schedule.start_ts = timestamp_us;
    op.io_schedule.batches.clear();
}

void HiCacheState::record_prefetch_batch(const HiCacheOperationHeader & operation, size_t index, const runtime::HiCacheIoBatchSchedule & batch) {
    auto & batches = scopes_.at(operation.cache_scope).async_ops.prefetch_ops().at(operation.operation_id).io_schedule.batches;
    if (index == batches.size()) batches.push_back(batch);
    else batches.at(index) = batch;
}

void HiCacheState::complete_prefetch_io(const HiCacheOperationHeader & operation, uint64_t timestamp_us) {
    auto & scope = scopes_.at(operation.cache_scope);
    auto & op = scope.async_ops.prefetch_ops().at(operation.operation_id);
    if (!execution_prefetch_control_ || !op.payload_transfer_issued || op.io_return_ts || timestamp_us < op.io_schedule.start_ts)
        throw std::logic_error("Prefetch I/O return requires completed physical work and a single return event");

    // A cancelled batch still entered storage with all requested pages. Keep
    // this issued volume separate from copied counts and published pages.
    uint64_t issued_pages = 0;
    for (const auto & batch : op.io_schedule.batches) issued_pages += batch.page_count;
    op.service_pages = prefix_to(op.hit_pages, issued_pages);
    op.io_schedule.effective_byte_count = core::checked_multiply_u64(issued_pages, config_.kv_bytes_per_page, "Prefetch executed bytes overflow");
    op.io_schedule.ready_ts = timestamp_us;
    op.io_schedule.duration_us = timestamp_us - op.io_schedule.start_ts;
    op.io_schedule.available = true;
    op.io_return_ts = timestamp_us;
    queue_prefetch_host_release(scope, op, op.hit_pages.size() - locally_published_pages(op));
}

HiCachePrefetchQueueSizes HiCacheState::prefetch_queue_sizes(const HiCacheFact & fact) {
    HiCachePrefetchQueueSizes sizes;
    const auto scope = scopes_.find(normalized_scope(fact));
    if (scope == scopes_.end()) return sizes;
    sizes.revoked_operations = scope->second.prefetch_revoke_queue.size();
    sizes.backup_acks = scope->second.backup_ack_queue.size();
    // SGLang append_host_mem_release splits tensors into one queue entry per
    // page and skips empty tensors. Our compressed entries store page counts,
    // so this sum is also the actual qsize sampled by the scheduler MIN.
    for (const auto & entry : scope->second.prefetch_host_release_queue) sizes.released_pages += entry.second;
    return sizes;
}

void HiCacheState::drain_prefetch_queues(const HiCacheFact & fact, HiCachePrefetchQueueSizes counts) {
    auto & scope = scope_state(fact);
    const auto available = prefetch_queue_sizes(fact);
    if (!execution_prefetch_control_ || counts.revoked_operations > available.revoked_operations || counts.released_pages > available.released_pages
        || counts.backup_acks > available.backup_acks)
        throw std::logic_error("Prefetch drain exceeds available queue work");
    for (uint64_t i = 0; i < counts.revoked_operations; ++i) {
        auto & op = scope.async_ops.prefetch_ops().at(scope.prefetch_revoke_queue.front());
        if (prefetch_active(op)) cancel_prefetch_pending_release(fact, scope, op, HiCachePrefetchState::Revoked);
        scope.prefetch_revoke_queue.pop_front();
    }
    for (uint64_t i = 0; i < counts.backup_acks; ++i) {
        const auto & op = scope.async_ops.storage_ops().at(scope.backup_ack_queue.front());
        const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        scope.backup_ack_queue.pop_front();
    }
    while (counts.released_pages) {
        auto & entry = scope.prefetch_host_release_queue.front();
        auto & op = scope.async_ops.prefetch_ops().at(entry.first);
        const auto pages = std::min(counts.released_pages, entry.second);
        op.reserved_host_pages = core::checked_subtract_u64(op.reserved_host_pages, pages, "Prefetch queued release exceeds its reservation");
        entry.second -= pages;
        counts.released_pages -= pages;
        if (!entry.second) scope.prefetch_host_release_queue.pop_front();
    }
    sync_capacity(scope, normalized_scope(fact), {});
}
} // namespace markov::trace_graph::modules::hicache::model
