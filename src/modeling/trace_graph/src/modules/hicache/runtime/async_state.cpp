/**
 * @file
 * @brief Target-derived HiCache asynchronous operation table implementation.
 */
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"

#include "markov/trace_graph/core/numeric.hpp"

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

template <typename OperationMap> void validate_new_operation(const HiCacheOperationHeader & header, const OperationMap & operations) {
    if (header.operation_id.empty()) throw std::invalid_argument("HiCache operation ID must not be empty");
    if (operations.contains(header.operation_id)) throw std::logic_error("Duplicate HiCache operation ID: " + header.operation_id);
}

} // namespace async_state_detail

using async_state_detail::prefetch_has_active_request_budget;
using async_state_detail::validate_new_operation;

void HiCacheAsyncOperationTable::transition_header(HiCacheOperationHeader & header, HiCacheOperationState state, uint64_t transition_ts) {
    if (header.state == state) return;
    // The lifecycle epoch is a model-local monotonic clock used by effect intents and
    // Debug ordering. It does not represent a source-runtime scheduling tick.
    const auto epoch = core::checked_increment_u64(lifecycle_epoch_, "HiCache operation lifecycle epoch exceeds uint64 range");
    if (state == HiCacheOperationState::Queued && header.enqueue_epoch == 0) header.enqueue_epoch = epoch;
    if ((state == HiCacheOperationState::Completed || state == HiCacheOperationState::Committed) && header.complete_epoch == 0) {
        header.complete_epoch = epoch;
        header.complete_ts = transition_ts;
    }
    header.state = state;
}

void HiCacheAsyncOperationTable::insert_prefetch(HiCachePrefetchOperation op) {
    // A request may receive multiple prefetch decisions. Point queries return the latest,
    // while the reverse index retains prior IDs until their reservations are drainable.
    if (op.header.request_key.empty()) throw std::invalid_argument("HiCache prefetch operation requires a request key");
    validate_new_operation(op.header, prefetch_by_id_);
    transition_header(op.header, HiCacheOperationState::Queued, op.header.enqueue_ts);
    const auto operation_id = op.header.operation_id;
    const auto request_key = op.header.request_key;
    prefetch_by_id_.emplace(operation_id, std::move(op));
    operation_ids_by_request_[request_key].push_back(operation_id);
}

HiCachePrefetchOperation * HiCacheAsyncOperationTable::prefetch_for_request(const std::string & request_key) {
    const auto request = operation_ids_by_request_.find(request_key);
    if (request == operation_ids_by_request_.end()) return nullptr;

    const auto op_it = prefetch_by_id_.find(request->second.back());
    return op_it == prefetch_by_id_.end() ? nullptr : &op_it->second;
}

const HiCachePrefetchOperation * HiCacheAsyncOperationTable::prefetch_for_request(const std::string & request_key) const {
    const auto request = operation_ids_by_request_.find(request_key);
    if (request == operation_ids_by_request_.end()) return nullptr;

    const auto op_it = prefetch_by_id_.find(request->second.back());
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
    // Cancellation does not free reservations; count them until an explicit drain releases them.
    for (const auto & op : prefetch_by_id_ | std::views::values) {
        if (op.header.cache_scope != cache_scope) continue;
        pages = core::checked_add_u64(pages, op.reserved_host_pages, "HiCache prefetch reservation pages exceed uint64 range");
    }
    for (const auto & [id, op] : storage_by_id_)
        if (op.header.cache_scope == cache_scope)
            pages = core::checked_add_u64(pages, op.reserved_host_pages, "HiCache write reservation pages exceed uint64 range");
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

void HiCacheAsyncOperationTable::set_prefetch_state_by_id(const std::string & operation_id, HiCachePrefetchState prefetch_state,
                                                          HiCacheOperationState operation_state, uint64_t transition_ts) {
    if (const auto it = prefetch_by_id_.find(operation_id); it != prefetch_by_id_.end()) {
        it->second.prefetch_state = prefetch_state;
        transition_header(it->second.header, operation_state, transition_ts);
    }
}

void HiCacheAsyncOperationTable::insert_loadback(HiCacheLoadbackOperation op) {
    validate_new_operation(op.header, loadback_by_id_);
    transition_header(op.header, HiCacheOperationState::Queued, op.header.enqueue_ts);
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

void HiCacheAsyncOperationTable::set_loadback_state(const std::string & operation_id, HiCacheOperationState state, uint64_t transition_ts) {
    if (const auto it = loadback_by_id_.find(operation_id); it != loadback_by_id_.end()) transition_header(it->second.header, state, transition_ts);
}

void HiCacheAsyncOperationTable::insert_storage(HiCacheStorageOperation op) {
    validate_new_operation(op.header, storage_by_id_);
    transition_header(op.header, HiCacheOperationState::Queued, op.header.enqueue_ts);
    const auto operation_id = op.header.operation_id;
    storage_by_id_.emplace(operation_id, std::move(op));
}

void HiCacheAsyncOperationTable::set_storage_state(const std::string & operation_id, HiCacheOperationState state, uint64_t transition_ts) {
    if (const auto it = storage_by_id_.find(operation_id); it != storage_by_id_.end()) transition_header(it->second.header, state, transition_ts);
}

void HiCacheAsyncOperationTable::set_storage_capacity_gate_pages(const std::string & operation_id, std::vector<std::string> pages) {
    const auto it = storage_by_id_.find(operation_id);
    if (it == storage_by_id_.end()) throw std::logic_error("Unknown HiCache storage operation: " + operation_id);
    it->second.capacity_gate_pages = std::move(pages);
}

void HiCacheAsyncOperationTable::set_storage_consumer_boundary(const std::string & operation_id, uint64_t consumer_epoch, uint64_t consumer_ts,
                                                               size_t source_node_id, std::optional<size_t> execution_anchor_node_id, size_t source_event_index,
                                                               std::string source_fact_role, bool source_available) {
    const auto it = storage_by_id_.find(operation_id);
    if (it == storage_by_id_.end()) throw std::logic_error("Unknown HiCache storage operation: " + operation_id);
    auto & header = it->second.header;
    if (header.consumer_epoch != 0) throw std::logic_error("HiCache storage operation already has a consumer boundary: " + operation_id);
    header.consumer_epoch = consumer_epoch;
    header.consumer_ts = consumer_ts;
    header.consumer_source_node_id = source_node_id;
    header.consumer_execution_anchor_node_id = execution_anchor_node_id;
    header.consumer_source_event_index = source_event_index;
    header.consumer_source_fact_role = std::move(source_fact_role);
    header.consumer_source_available = source_available;
}

HiCacheStorageOperation * HiCacheAsyncOperationTable::storage_operation(const std::string & operation_id) {
    const auto it = storage_by_id_.find(operation_id);
    return it == storage_by_id_.end() ? nullptr : &it->second;
}

const HiCacheStorageOperation * HiCacheAsyncOperationTable::storage_operation(const std::string & operation_id) const {
    const auto it = storage_by_id_.find(operation_id);
    return it == storage_by_id_.end() ? nullptr : &it->second;
}

void HiCacheAsyncOperationTable::clear_operations_for_window_boundary() {
    prefetch_by_id_.clear();
    loadback_by_id_.clear();
    latest_loadback_id_by_request_.clear();
    storage_by_id_.clear();
    operation_ids_by_request_.clear();
}

} // namespace markov::trace_graph::modules::hicache::runtime
