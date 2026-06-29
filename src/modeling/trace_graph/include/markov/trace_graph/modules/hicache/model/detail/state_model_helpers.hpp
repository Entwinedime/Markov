/**
 * @file
 * @brief HiCache state model 内部共享 helper。
 *
 * 本头文件属于 model/detail 层，只服务状态机拆分后的多个 handler；新增 helper
 * 必须先确认确有多 caller 需求，短小单 caller 逻辑应保留在调用点附近。
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

namespace detail {

/** @brief 生成小写副本，用于宽松匹配配置和 trace 字段枚举值。 */
inline std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

/** @brief 将 node chain 对应的压缩 page group 展平成 page 序列。 */
inline std::vector<std::string> flatten_node_pages(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & nodes) {
    std::vector<std::string> pages;
    std::ranges::for_each(nodes, [&](auto node_id) {
        auto node_pages = tree.node_pages(node_id);
        pages.insert(pages.end(), node_pages.begin(), node_pages.end());
    });
    return pages;
}

/** @brief 判断 node 是否已经有 host-visible backup。 */
inline bool has_host_backup(const HiCacheCacheNode & node) { return node.residency.host_present; }

/** @brief 返回 vector 从 begin 起的 suffix，越界时返回空 vector。 */
template <typename T> std::vector<T> suffix_from(const std::vector<T> & values, size_t begin) {
    if (begin >= values.size()) return {};
    std::vector<T> result;
    result.reserve(values.size() - begin);
    auto view = values | std::views::drop(static_cast<std::ranges::range_difference_t<std::vector<T>>>(begin));
    std::ranges::copy(view, std::back_inserter(result));
    return result;
}

/** @brief 返回 page 序列前 end 个元素，end 超过长度时截到末尾。 */
inline std::vector<std::string> prefix_to(const std::vector<std::string> & pages, size_t end) {
    end = std::min(end, pages.size());
    std::vector<std::string> result;
    result.reserve(end);
    auto view = pages | std::views::take(static_cast<std::ranges::range_difference_t<std::vector<std::string>>>(end));
    std::ranges::copy(view, std::back_inserter(result));
    return result;
}

/** @brief 根据 storage directory 计算 planned pages 中连续可读的 prefix。 */
inline std::vector<std::string> storage_hit_prefix(const HiCacheStorageDirectory & storage, const std::vector<HiCacheProjectedPage> & planned_pages) {
    return storage.contiguous_readable_prefix(planned_pages);
}

/** @brief 判断 prefetch operation 是否仍占用 active requested budget。 */
inline bool prefetch_active(const HiCachePrefetchOperation & op) {
    return op.prefetch_state == HiCachePrefetchState::Pending || op.prefetch_state == HiCachePrefetchState::Ready;
}

/** @brief 优先使用 target config page size，缺失时回退到 fact 携带的 source page size。 */
inline uint64_t effective_page_size(const HiCacheConfig & config, const HiCacheFact & fact) {
    return config.page_size > 0 ? config.page_size : fact.source_page_size;
}

/** @brief 生成 scope/request 复合 key，作为 request-local 状态索引。 */
inline std::string scoped_fact_request_key(const HiCacheFact & fact) {
    if (fact.request_id.empty()) return "";
    return (fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope) + ":" + fact.request_id;
}

/** @brief 判断 fact 是否满足当前 HiCache state model 的可消费合同。 */
inline bool ready_state_model_fact(const HiCacheFact & fact, const HiCacheFactRoute & route, const HiCacheConfig & config) {
    if (!route.model_fact || !route.known_role || !hicache_fact_role_implemented(route.role)) return false;
    return hicache_required_fact_errors(fact, route.role, effective_page_size(config, fact)).empty();
}

/** @brief 无符号整数向上取整除法；divisor 为 0 时返回 0。 */
inline uint64_t ceil_div(uint64_t value, uint64_t divisor) {
    if (divisor == 0) return 0;
    return value / divisor + (value % divisor == 0 ? 0 : 1);
}

/**
 * @brief 一次 SGLang extend allocator batch 的语义化输入。
 *
 * 当前 trace 还没有 `ScheduleBatch` 粒度 state-model fact，因此调用方显式传入
 * resolved policy 中的 `batch_size=1`。结构体保留 batch 语义字段，后续接入
 * `extend_allocation_intent` 后只需要替换构造来源，不需要重写 capacity 链路。
 */
struct ExtendAllocationIntent {
    uint64_t batch_size = 1;
    uint64_t page_size = 0;
    uint64_t seq_tokens = 0;
    uint64_t prefix_tokens = 0;
    uint64_t extend_tokens = 0;
    uint64_t requested_tokens = 0;
    uint64_t requested_pages = 0;
    uint64_t allocated_pages = 0;

    /** @brief 本轮是否会形成 allocator pressure。 */
    [[nodiscard]] bool needs_pressure() const { return requested_pages > 0; }
};

/**
 * @brief 计算 SGLang paged extend 的 eviction pressure token 数。
 *
 * `alloc_paged_token_slots_extend()` 对 paged allocator 使用
 * `extend_num_tokens + len(seq_lens_cpu) * page_size`。当 `page_size == 1` 时，
 * SGLang 走 non-paged `alloc_token_slots()`，没有每 request 一页的保守预算。
 */
inline uint64_t extend_requested_tokens(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size) {
    if (extend_tokens == 0 || page_size == 0) return 0;
    if (page_size == 1) return extend_tokens;
    return extend_tokens + batch_size * page_size;
}

/**
 * @brief 计算本次 extend 真正占用的新 page 数。
 *
 * 该值对应 SGLang `get_num_new_pages(seq_lens, prefix_lens)`，不包含 allocator
 * 为 eviction gate 额外加入的 conservative batch overhead。
 */
inline uint64_t extend_allocated_pages(uint64_t seq_tokens, uint64_t prefix_tokens, uint64_t page_size) {
    if (seq_tokens == 0 || page_size == 0) return 0;
    const auto bounded_prefix_tokens = std::min(prefix_tokens, seq_tokens);
    const auto pages_after = ceil_div(seq_tokens, page_size);
    const auto pages_before = ceil_div(bounded_prefix_tokens, page_size);
    return pages_after > pages_before ? pages_after - pages_before : 0;
}

/**
 * @brief 用当前显式 single-request batch 合同构造 extend allocation intent。
 */
inline ExtendAllocationIntent make_extend_allocation_intent(uint64_t token_count, uint64_t prefix_tokens, uint64_t page_size, uint64_t batch_size) {
    auto intent = ExtendAllocationIntent{
        .batch_size = batch_size == 0 ? uint64_t{ 1 } : batch_size,
        .page_size = page_size,
        .seq_tokens = token_count,
    };
    if (token_count == 0 || page_size == 0) return intent;

    intent.prefix_tokens = std::min(token_count, prefix_tokens);
    intent.extend_tokens = token_count - intent.prefix_tokens;
    intent.requested_tokens = extend_requested_tokens(intent.extend_tokens, intent.batch_size, page_size);
    intent.requested_pages = ceil_div(intent.requested_tokens, page_size);
    intent.allocated_pages = extend_allocated_pages(intent.seq_tokens, intent.prefix_tokens, page_size);
    return intent;
}

/**
 * @brief 计算 SGLang allocator 语义下本轮需要真实清理的 page 数。
 *
 * SGLang 在 device/host allocator 当前可用空间不足时才调用 radix eviction；
 * 一旦触发，预算使用本次 allocation request 大小，而不是仅清理 free-space deficit。
 * 如果没有 allocation request，则只清理当前已经超过 target capacity 的部分。
 */
inline uint64_t allocation_cleanup_target(uint64_t occupied_pages, uint64_t reserved_pages, uint64_t capacity_pages, uint64_t requested_pages) {
    if (capacity_pages == 0) return 0;

    const auto committed_pages = occupied_pages + reserved_pages;
    const auto excess_pages = committed_pages > capacity_pages ? committed_pages - capacity_pages : 0;
    if (requested_pages == 0) return excess_pages;

    const auto free_pages = committed_pages < capacity_pages ? capacity_pages - committed_pages : 0;
    if (free_pages >= requested_pages) return excess_pages;
    return std::max(excess_pages, requested_pages);
}

/** @brief 饱和减法，避免无符号下溢。 */
inline uint64_t bounded_subtract(uint64_t value, uint64_t decrement) { return decrement >= value ? 0 : value - decrement; }

/** @brief 统一构造 target-derived async operation header。 */
inline HiCacheOperationHeader make_operation_header(HiCacheOperationKind kind, const std::string & operation_id, const std::string & cache_scope,
                                                    const std::string & request_key, const std::string & owner, HiCacheNodeId anchor_node,
                                                    const std::vector<HiCacheNodeId> & node_ids, const std::vector<std::string> & pages, uint64_t enqueue_ts,
                                                    uint64_t enqueue_epoch) {
    return HiCacheOperationHeader{
        .operation_id = operation_id,
        .kind = kind,
        .cache_scope = cache_scope,
        .request_key = request_key,
        .owner = owner,
        .anchor_node = anchor_node,
        .node_ids = node_ids,
        .pages = pages,
        .enqueue_epoch = enqueue_epoch,
        .enqueue_ts = enqueue_ts,
    };
}

/** @brief 生成 request lifecycle 使用的 ordinary lock ref owner 名称。 */
inline std::string request_ref_owner(const std::string & request_key) { return request_key.empty() ? std::string{} : request_key + ":request"; }

/** @brief 从 storage page id/hash 复合值中提取 backend hash 部分。 */
inline std::string storage_hash_from_fact_value(const std::string & value) {
    const auto delimiter = value.find('|');
    if (delimiter == std::string::npos) return value;
    return value.substr(delimiter + 1);
}

} // namespace detail

using detail::allocation_cleanup_target;
using detail::bounded_subtract;
using detail::ceil_div;
using detail::effective_page_size;
using detail::ExtendAllocationIntent;
using detail::flatten_node_pages;
using detail::has_host_backup;
using detail::lower_copy;
using detail::make_extend_allocation_intent;
using detail::make_operation_header;
using detail::prefetch_active;
using detail::prefix_to;
using detail::ready_state_model_fact;
using detail::request_ref_owner;
using detail::scoped_fact_request_key;
using detail::storage_hash_from_fact_value;
using detail::storage_hit_prefix;
using detail::suffix_from;

} // namespace markov::trace_graph::modules::hicache::model
