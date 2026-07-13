/**
 * @file
 * @brief Target-derived HiCache asynchronous operation table implementation.
 */
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace async_state_detail {

/**
 * @brief Returns whether a prefetch still consumes SGLang-style request budget.
 *
 * Ready remains active because the result has not yet materialized at a request boundary.
 */
bool prefetch_has_active_request_budget(const HiCachePrefetchOperation & op) {
    return op.prefetch_state == HiCachePrefetchState::Pending || op.prefetch_state == HiCachePrefetchState::Ready;
}

/**
 * @brief Returns whether a prefetch reservation still contributes L2 pressure.
 *
 * Failed or cancelled operations retain their reservation until a target-derived drain
 * boundary or finalization explicitly releases it.
 */
bool prefetch_occupies_host_budget(const HiCachePrefetchOperation & op) {
    if (op.reserved_host_pages == 0) return false;
    return op.prefetch_state == HiCachePrefetchState::Pending || op.prefetch_state == HiCachePrefetchState::Ready
           || op.prefetch_state == HiCachePrefetchState::Applied || op.prefetch_state == HiCachePrefetchState::Suppressed
           || op.prefetch_state == HiCachePrefetchState::Late || op.prefetch_state == HiCachePrefetchState::Revoked;
}

template <typename OperationMap>
void validate_new_operation(const HiCacheOperationHeader & header, HiCacheOperationKind expected_kind, const OperationMap & operations) {
    if (header.operation_id.empty()) throw std::invalid_argument("HiCache operation ID must not be empty");
    if (header.kind != expected_kind) throw std::invalid_argument("HiCache operation kind does not match its operation table");
    if (operations.contains(header.operation_id)) throw std::logic_error("Duplicate HiCache operation ID: " + header.operation_id);
}

} // namespace async_state_detail

using async_state_detail::prefetch_has_active_request_budget;
using async_state_detail::prefetch_occupies_host_budget;
using async_state_detail::validate_new_operation;

void HiCacheAsyncOperationTable::index_prefetch(const HiCacheOperationHeader & header) {
    // Reservation drain is the only production reverse-index consumer. Indexing other
    // operation kinds or nodes retained duplicate strings without changing behavior.
    if (!header.request_key.empty()) {
        auto & request_ops = operation_ids_by_request_[header.request_key];
        if (std::ranges::find(request_ops, header.operation_id) == request_ops.end()) request_ops.push_back(header.operation_id);
    }
}

void HiCacheAsyncOperationTable::transition_header(HiCacheOperationHeader & header, HiCacheOperationState state, std::string_view reason,
                                                   uint64_t transition_ts) {
    if (header.state == state) return;
    // The lifecycle epoch is a model-local monotonic clock used by effect intents and
    // Debug ordering. It does not represent a source-runtime scheduling tick.
    const auto epoch = core::checked_increment_u64(lifecycle_epoch_, "HiCache operation lifecycle epoch exceeds uint64 range");
#ifdef DEBUG
    lifecycle_transitions_.push_back(HiCacheOperationLifecycleTransition{
        .operation_id = header.operation_id,
        .kind = header.kind,
        .from_state = header.state,
        .to_state = state,
        .transition_epoch = epoch,
        .transition_ts = transition_ts,
        .cache_scope = header.cache_scope,
        .request_key = header.request_key,
        .reason = std::string(reason),
    });
#else
    (void)reason;
#endif
    if (state == HiCacheOperationState::Queued && header.enqueue_epoch == 0) header.enqueue_epoch = epoch;
    if (state == HiCacheOperationState::Completed && header.complete_epoch == 0) {
        header.complete_epoch = epoch;
        header.complete_ts = transition_ts;
    }
    if (state == HiCacheOperationState::Committed) {
        if (header.complete_epoch == 0) {
            header.complete_epoch = epoch;
            header.complete_ts = transition_ts;
        }
    }
    header.state = state;
}

void HiCacheAsyncOperationTable::insert_prefetch(HiCachePrefetchOperation op) {
    // A request may receive multiple prefetch decisions. Point queries return the latest,
    // while the reverse index retains prior IDs until their reservations are drainable.
    if (op.header.request_key.empty()) throw std::invalid_argument("HiCache prefetch operation requires a request key");
    validate_new_operation(op.header, HiCacheOperationKind::Prefetch, prefetch_by_id_);
    transition_header(op.header, HiCacheOperationState::Queued, "enqueue_prefetch", op.header.enqueue_ts);
    const auto operation_id = op.header.operation_id;
    const auto request_key = op.header.request_key;
    auto [it, inserted] = prefetch_by_id_.emplace(operation_id, std::move(op));
    if (!inserted) throw std::logic_error("Duplicate HiCache prefetch operation ID: " + operation_id);
    index_prefetch(it->second.header);
    latest_prefetch_id_by_request_[request_key] = operation_id;
}

HiCachePrefetchOperation * HiCacheAsyncOperationTable::prefetch_for_request(const std::string & request_key) {
    const auto id_it = latest_prefetch_id_by_request_.find(request_key);
    if (id_it == latest_prefetch_id_by_request_.end()) return nullptr;
    const auto op_it = prefetch_by_id_.find(id_it->second);
    return op_it == prefetch_by_id_.end() ? nullptr : &op_it->second;
}

const HiCachePrefetchOperation * HiCacheAsyncOperationTable::prefetch_for_request(const std::string & request_key) const {
    const auto id_it = latest_prefetch_id_by_request_.find(request_key);
    if (id_it == latest_prefetch_id_by_request_.end()) return nullptr;
    const auto op_it = prefetch_by_id_.find(id_it->second);
    return op_it == prefetch_by_id_.end() ? nullptr : &op_it->second;
}

uint64_t HiCacheAsyncOperationTable::active_requested_pages(const std::string & cache_scope) const {
    uint64_t pages = 0;
    for (const auto & op : prefetch_by_id_ | std::views::values) {
        if (op.header.cache_scope != cache_scope || !prefetch_has_active_request_budget(op)) continue;
        pages = core::checked_add_u64(pages, op.requested_host_pages, "HiCache active prefetch request pages exceed uint64 range");
    }
    return pages;
}

uint64_t HiCacheAsyncOperationTable::reserved_pages(const std::string & cache_scope) const {
    uint64_t pages = 0;
    for (const auto & op : prefetch_by_id_ | std::views::values) {
        if (op.header.cache_scope != cache_scope || !prefetch_occupies_host_budget(op)) continue;
        pages = core::checked_add_u64(pages, op.reserved_host_pages, "HiCache prefetch reservation pages exceed uint64 range");
    }
    return pages;
}

uint64_t HiCacheAsyncOperationTable::release_prefetch_pending_host_pages_for_request(const std::string & request_key) {
    // Pending and ready prefetches may still be consumed by the request. Drain only
    // reservations that no longer count as active request budget.
    const auto request_ops = operation_ids_by_request_.find(request_key);
    if (request_ops == operation_ids_by_request_.end()) return 0;
    uint64_t pages = 0;
    for (const auto & operation_id : request_ops->second) {
        auto op = prefetch_by_id_.find(operation_id);
        if (op == prefetch_by_id_.end() || prefetch_has_active_request_budget(op->second)) continue;
        pages = core::checked_add_u64(pages, op->second.reserved_host_pages, "HiCache released prefetch pages exceed uint64 range");
        op->second.reserved_host_pages = 0;
    }
    return pages;
}

void HiCacheAsyncOperationTable::set_prefetch_state(const std::string & request_key, HiCachePrefetchState prefetch_state, HiCacheOperationState operation_state,
                                                    std::string_view reason, uint64_t transition_ts) {
    if (auto * op = prefetch_for_request(request_key); op != nullptr) {
        set_prefetch_state_by_id(op->header.operation_id, prefetch_state, operation_state, reason, transition_ts);
    }
}

void HiCacheAsyncOperationTable::set_prefetch_state_by_id(const std::string & operation_id, HiCachePrefetchState prefetch_state,
                                                          HiCacheOperationState operation_state, std::string_view reason, uint64_t transition_ts) {
    if (const auto it = prefetch_by_id_.find(operation_id); it != prefetch_by_id_.end()) {
        it->second.prefetch_state = prefetch_state;
        transition_header(it->second.header, operation_state, reason, transition_ts);
    }
}

void HiCacheAsyncOperationTable::insert_writeback(HiCacheWritebackOperation op) {
    validate_new_operation(op.header, HiCacheOperationKind::Writeback, writeback_by_id_);
    transition_header(op.header, HiCacheOperationState::Queued, "enqueue_writeback", op.header.enqueue_ts);
    const auto operation_id = op.header.operation_id;
    writeback_by_id_.emplace(operation_id, std::move(op));
}

void HiCacheAsyncOperationTable::set_writeback_state(const std::string & operation_id, HiCacheOperationState state, std::string_view reason,
                                                     uint64_t transition_ts) {
    if (const auto it = writeback_by_id_.find(operation_id); it != writeback_by_id_.end()) transition_header(it->second.header, state, reason, transition_ts);
}

void HiCacheAsyncOperationTable::insert_loadback(HiCacheLoadbackOperation op) {
    validate_new_operation(op.header, HiCacheOperationKind::Loadback, loadback_by_id_);
    transition_header(op.header, HiCacheOperationState::Queued, "enqueue_loadback", op.header.enqueue_ts);
    const auto operation_id = op.header.operation_id;
    const auto request_key = op.header.request_key;
    loadback_by_id_.emplace(operation_id, std::move(op));
    if (!request_key.empty()) latest_loadback_id_by_request_[request_key] = operation_id;
}

HiCacheLoadbackOperation * HiCacheAsyncOperationTable::loadback_for_request(const std::string & request_key) {
    const auto id = latest_loadback_id_by_request_.find(request_key);
    if (id == latest_loadback_id_by_request_.end()) return nullptr;
    const auto operation = loadback_by_id_.find(id->second);
    return operation == loadback_by_id_.end() ? nullptr : &operation->second;
}

const HiCacheLoadbackOperation * HiCacheAsyncOperationTable::loadback_for_request(const std::string & request_key) const {
    const auto id = latest_loadback_id_by_request_.find(request_key);
    if (id == latest_loadback_id_by_request_.end()) return nullptr;
    const auto operation = loadback_by_id_.find(id->second);
    return operation == loadback_by_id_.end() ? nullptr : &operation->second;
}

void HiCacheAsyncOperationTable::set_loadback_state(const std::string & operation_id, HiCacheOperationState state, std::string_view reason,
                                                    uint64_t transition_ts) {
    if (const auto it = loadback_by_id_.find(operation_id); it != loadback_by_id_.end()) transition_header(it->second.header, state, reason, transition_ts);
}

void HiCacheAsyncOperationTable::insert_storage(HiCacheStorageOperation op) {
    validate_new_operation(op.header, HiCacheOperationKind::Storage, storage_by_id_);
    transition_header(op.header, HiCacheOperationState::Queued, "enqueue_storage", op.header.enqueue_ts);
    const auto operation_id = op.header.operation_id;
    storage_by_id_.emplace(operation_id, std::move(op));
}

void HiCacheAsyncOperationTable::set_storage_state(const std::string & operation_id, HiCacheOperationState state, std::string_view reason,
                                                   uint64_t transition_ts) {
    if (const auto it = storage_by_id_.find(operation_id); it != storage_by_id_.end()) transition_header(it->second.header, state, reason, transition_ts);
}

void HiCacheAsyncOperationTable::set_storage_capacity_gate_pages(const std::string & operation_id, std::vector<std::string> pages) {
    const auto it = storage_by_id_.find(operation_id);
    if (it == storage_by_id_.end()) throw std::logic_error("Unknown HiCache storage operation: " + operation_id);
    it->second.capacity_gate_pages = std::move(pages);
}

void HiCacheAsyncOperationTable::set_storage_consumer_boundary(const std::string & operation_id, uint64_t consumer_epoch, uint64_t consumer_ts,
                                                               size_t source_node_id, size_t source_event_index, std::string source_fact_role,
                                                               bool source_available) {
    const auto it = storage_by_id_.find(operation_id);
    if (it == storage_by_id_.end()) throw std::logic_error("Unknown HiCache storage operation: " + operation_id);
    auto & header = it->second.header;
    if (header.consumer_epoch != 0) throw std::logic_error("HiCache storage operation already has a consumer boundary: " + operation_id);
    header.consumer_epoch = consumer_epoch;
    header.consumer_ts = consumer_ts;
    header.consumer_source_node_id = source_node_id;
    header.consumer_source_event_index = source_event_index;
    header.consumer_source_fact_role = std::move(source_fact_role);
    header.consumer_source_available = source_available;
}

std::span<const std::string> HiCacheAsyncOperationTable::operations_for_request(const std::string & request_key) const {
    const auto it = operation_ids_by_request_.find(request_key);
    return it == operation_ids_by_request_.end() ? std::span<const std::string>{} : std::span<const std::string>{ it->second };
}

} // namespace markov::trace_graph::modules::hicache::runtime
