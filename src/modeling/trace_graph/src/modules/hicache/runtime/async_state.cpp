/**
 * @file
 * @brief HiCache async operation 表实现。
 */
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace async_state_detail {

/**
 * @brief active_requested_pages 近似 SGLang rate-limit 中仍在请求 host 空间的 prefetch 规模。
 *
 * ready 仍算 active，因为它尚未在 request 使用边界 materialize。
 */
bool prefetch_has_active_request_budget(const HiCachePrefetchOperation & op) {
    return op.prefetch_state == HiCachePrefetchState::Pending || op.prefetch_state == HiCachePrefetchState::Ready;
}

/**
 * @brief reserved_host_pages 表示 L2 capacity pressure。
 *
 * prefetch 失败/取消后不会立即消失，需要等 target-derived drain boundary 或 finalize 回收。
 */
bool prefetch_occupies_host_budget(const HiCachePrefetchOperation & op) {
    if (op.reserved_host_pages == 0) return false;
    return op.prefetch_state == HiCachePrefetchState::Pending || op.prefetch_state == HiCachePrefetchState::Ready
           || op.prefetch_state == HiCachePrefetchState::Applied || op.prefetch_state == HiCachePrefetchState::Suppressed
           || op.prefetch_state == HiCachePrefetchState::Late || op.prefetch_state == HiCachePrefetchState::Revoked;
}

} // namespace async_state_detail

using async_state_detail::prefetch_has_active_request_budget;
using async_state_detail::prefetch_occupies_host_budget;

void HiCacheAsyncOperationTable::index_operation(const HiCacheOperationHeader & header) {
    /**
     * @brief operation 同时按 request 和 node 建索引。
     *
     * request 维度服务 prefetch release drain，node 维度服务 diagnostics 和 ref/capacity 解释。
     */
    if (!header.request_key.empty()) {
        auto & request_ops = operation_ids_by_request_[header.request_key];
        if (std::ranges::find(request_ops, header.operation_id) == request_ops.end()) request_ops.push_back(header.operation_id);
    }
    for (const auto node_id : header.node_ids) {
        auto & node_ops = operation_ids_by_node_[node_id];
        if (std::ranges::find(node_ops, header.operation_id) == node_ops.end()) node_ops.push_back(header.operation_id);
    }
}

void HiCacheAsyncOperationTable::transition_header(HiCacheOperationHeader & header, HiCacheOperationState state, const std::string & reason,
                                                   uint64_t transition_ts) {
    if (header.state == state) return;
    /**
     * @brief lifecycle epoch 是模型内部单调时钟。
     *
     * 它用来解释 async op 的排队、ready、commit/cancel 顺序；不代表 source runtime
     * 线程的真实调度 tick。
     */
    const auto epoch = ++lifecycle_epoch_;
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
        .reason = reason,
    });
#endif
    if (state == HiCacheOperationState::Ready && header.eligible_epoch == 0) header.eligible_epoch = epoch;
    if (state == HiCacheOperationState::Completed && header.complete_epoch == 0) {
        header.complete_epoch = epoch;
        header.complete_ts = transition_ts;
    }
    if (state == HiCacheOperationState::Committed) {
        if (header.complete_epoch == 0) {
            header.complete_epoch = epoch;
            header.complete_ts = transition_ts;
        }
        header.commit_epoch = epoch;
        header.commit_ts = transition_ts;
    }
    if (state == HiCacheOperationState::Cancelled) {
        header.cancel_reason = reason;
        header.cancel_ts = transition_ts;
    }
    header.state = state;
}

void HiCacheAsyncOperationTable::upsert_prefetch(HiCachePrefetchOperation op) {
    /**
     * @brief 同一 request 可能出现多次 prefetch decision。
     *
     * 普通查询只返回最新 operation，但较早 operation 仍保留在 request 索引里，便于后续释放它遗留的 reservation。
     */
    index_operation(op.header);
    transition_header(op.header, HiCacheOperationState::Queued, "enqueue_prefetch", op.header.enqueue_ts);
    latest_prefetch_id_by_request_[op.header.request_key] = op.header.operation_id;
    prefetch_by_id_[op.header.operation_id] = std::move(op);
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
        pages += op.requested_host_pages;
    }
    return pages;
}

uint64_t HiCacheAsyncOperationTable::reserved_pages(const std::string & cache_scope) const {
    uint64_t pages = 0;
    for (const auto & op : prefetch_by_id_ | std::views::values) {
        if (op.header.cache_scope != cache_scope || !prefetch_occupies_host_budget(op)) continue;
        pages += op.reserved_host_pages;
    }
    return pages;
}

uint64_t HiCacheAsyncOperationTable::release_prefetch_pending_host_pages_for_request(const std::string & request_key) {
    /**
     * @brief 只回收已经不再 active-request-budget 的 reservation。
     *
     * pending/ready prefetch 仍可能被当前 request 使用，不能在 drain 时提前释放。
     */
    const auto request_ops = operation_ids_by_request_.find(request_key);
    if (request_ops == operation_ids_by_request_.end()) return 0;
    uint64_t pages = 0;
    for (const auto & operation_id : request_ops->second) {
        auto op = prefetch_by_id_.find(operation_id);
        if (op == prefetch_by_id_.end() || prefetch_has_active_request_budget(op->second)) continue;
        pages += op->second.reserved_host_pages;
        op->second.reserved_host_pages = 0;
    }
    return pages;
}

void HiCacheAsyncOperationTable::set_prefetch_state(const std::string & request_key, HiCachePrefetchState prefetch_state, HiCacheOperationState operation_state,
                                                    const std::string & reason, uint64_t transition_ts) {
    if (auto * op = prefetch_for_request(request_key); op != nullptr) {
        set_prefetch_state_by_id(op->header.operation_id, prefetch_state, operation_state, reason, transition_ts);
    }
}

void HiCacheAsyncOperationTable::set_prefetch_state_by_id(const std::string & operation_id, HiCachePrefetchState prefetch_state,
                                                          HiCacheOperationState operation_state, const std::string & reason, uint64_t transition_ts) {
    if (const auto it = prefetch_by_id_.find(operation_id); it != prefetch_by_id_.end()) {
        it->second.prefetch_state = prefetch_state;
        transition_header(it->second.header, operation_state, reason, transition_ts);
    }
}

void HiCacheAsyncOperationTable::upsert_writeback(HiCacheWritebackOperation op) {
    index_operation(op.header);
    transition_header(op.header, HiCacheOperationState::Queued, "enqueue_writeback", op.header.enqueue_ts);
    writeback_by_id_[op.header.operation_id] = std::move(op);
}

void HiCacheAsyncOperationTable::set_writeback_state(const std::string & operation_id, HiCacheOperationState state, const std::string & reason,
                                                     uint64_t transition_ts) {
    if (const auto it = writeback_by_id_.find(operation_id); it != writeback_by_id_.end()) transition_header(it->second.header, state, reason, transition_ts);
}

void HiCacheAsyncOperationTable::upsert_loadback(HiCacheLoadbackOperation op) {
    index_operation(op.header);
    transition_header(op.header, HiCacheOperationState::Queued, "enqueue_loadback", op.header.enqueue_ts);
    loadback_by_id_[op.header.operation_id] = std::move(op);
}

void HiCacheAsyncOperationTable::set_loadback_state(const std::string & operation_id, HiCacheOperationState state, const std::string & reason,
                                                    uint64_t transition_ts) {
    if (const auto it = loadback_by_id_.find(operation_id); it != loadback_by_id_.end()) transition_header(it->second.header, state, reason, transition_ts);
}

void HiCacheAsyncOperationTable::upsert_storage(HiCacheStorageOperation op) {
    index_operation(op.header);
    transition_header(op.header, HiCacheOperationState::Queued, "enqueue_storage", op.header.enqueue_ts);
    storage_by_id_[op.header.operation_id] = std::move(op);
}

void HiCacheAsyncOperationTable::set_storage_state(const std::string & operation_id, HiCacheOperationState state, const std::string & reason,
                                                   uint64_t transition_ts) {
    if (const auto it = storage_by_id_.find(operation_id); it != storage_by_id_.end()) transition_header(it->second.header, state, reason, transition_ts);
}

std::vector<std::string> HiCacheAsyncOperationTable::operations_for_request(const std::string & request_key) const {
    const auto it = operation_ids_by_request_.find(request_key);
    return it == operation_ids_by_request_.end() ? std::vector<std::string>{} : it->second;
}

std::vector<std::string> HiCacheAsyncOperationTable::operations_for_node(HiCacheNodeId node_id) const {
    const auto it = operation_ids_by_node_.find(node_id);
    return it == operation_ids_by_node_.end() ? std::vector<std::string>{} : it->second;
}

} // namespace markov::trace_graph::modules::hicache::runtime
