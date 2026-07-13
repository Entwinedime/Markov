/**
 * @file
 * @brief Shared implementation helpers for the HiCache state model.
 *
 * This header is private to the model/detail layer and serves handlers extracted
 * from the state aggregate. Add a helper only when multiple callers share the same
 * decision; short single-caller logic should remain at its call site.
 */
#pragma once

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

namespace detail {

/** @brief Flattens compressed page groups from a radix-node chain. */
inline std::vector<std::string> flatten_node_pages(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & nodes) {
    std::vector<std::string> pages;
    std::ranges::for_each(nodes, [&](auto node_id) {
        const auto & node_pages = tree.node_pages(node_id);
        pages.insert(pages.end(), node_pages.begin(), node_pages.end());
    });
    return pages;
}

/** @brief Reports whether a radix node already has a host-visible backup. */
inline bool has_host_backup(const radix::HiCacheCacheNode & node) { return node.residency.host_present; }

/** @brief Copies the suffix beginning at `begin`, or returns empty when out of range. */
template <typename T> std::vector<T> suffix_from(const std::vector<T> & values, size_t begin) {
    if (begin >= values.size()) return {};
    std::vector<T> result;
    result.reserve(values.size() - begin);
    auto view = values | std::views::drop(static_cast<std::ranges::range_difference_t<std::vector<T>>>(begin));
    std::ranges::copy(view, std::back_inserter(result));
    return result;
}

/** @brief Copies at most the first `end` pages. */
inline std::vector<std::string> prefix_to(const std::vector<std::string> & pages, size_t end) {
    end = std::min(end, pages.size());
    std::vector<std::string> result;
    result.reserve(end);
    auto view = pages | std::views::take(static_cast<std::ranges::range_difference_t<std::vector<std::string>>>(end));
    std::ranges::copy(view, std::back_inserter(result));
    return result;
}

/** @brief Reports whether a prefetch still consumes active requested capacity. */
inline bool prefetch_active(const HiCachePrefetchOperation & op) {
    return op.prefetch_state == HiCachePrefetchState::Pending || op.prefetch_state == HiCachePrefetchState::Ready;
}

/** @brief Computes unsigned ceiling division, returning zero for a zero divisor. */
inline uint64_t ceil_div(uint64_t value, uint64_t divisor) {
    if (divisor == 0) return 0;
    return value / divisor + (value % divisor == 0 ? 0 : 1);
}

/**
 * @brief Semantic allocation input for one SGLang extend batch.
 *
 * A `cache_extend_input` fact represents the complete batch. Callers use the real
 * batch size when reconstructing eviction pressure, or one when calculating only
 * the pages newly occupied by a single request.
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

    /** @brief Reports whether this extend creates allocator pressure. */
    [[nodiscard]] bool needs_pressure() const { return requested_pages > 0; }
};

/** @brief Raw token dimensions used to derive one extend-allocation intent. */
struct ExtendAllocationInput {
    uint64_t token_count = 0;
    uint64_t prefix_tokens = 0;
    uint64_t page_size = 0;
    uint64_t batch_size = 1;
};

/** @brief Token pressure dimensions consumed by SGLang's allocation gate. */
struct ExtendPressureInput {
    uint64_t extend_tokens = 0;
    uint64_t batch_size = 1;
    uint64_t page_size = 0;
};

/** @brief Capacity dimensions used to derive one host cleanup budget. */
struct HostCleanupInput {
    uint64_t occupied_pages = 0;
    uint64_t reserved_pages = 0;
    uint64_t capacity_pages = 0;
    uint64_t requested_pages = 0;
};

/**
 * @brief Reconstructs the token pressure used by SGLang paged extend allocation.
 *
 * `alloc_paged_token_slots_extend()` uses
 * `extend_num_tokens + len(seq_lens_cpu) * page_size`. With `page_size == 1`,
 * SGLang uses non-paged `alloc_token_slots()` and omits per-request overhead.
 */
inline uint64_t extend_requested_tokens(const ExtendPressureInput & input) {
    if (input.extend_tokens == 0 || input.page_size == 0) return 0;
    if (input.page_size == 1) return input.extend_tokens;
    const auto batch_overhead = core::checked_multiply_u64(input.batch_size, input.page_size, "HiCache extend allocation overhead exceeds uint64 range");
    return core::checked_add_u64(input.extend_tokens, batch_overhead, "HiCache extend allocation pressure exceeds uint64 range");
}

/**
 * @brief Computes the number of pages actually occupied by this extend.
 *
 * This corresponds to SGLang `get_num_new_pages(seq_lens, prefix_lens)` and excludes
 * the conservative batch overhead used only by the allocator eviction gate.
 */
inline uint64_t extend_allocated_pages(const ExtendAllocationIntent & intent) {
    if (intent.seq_tokens == 0 || intent.page_size == 0) return 0;
    const auto bounded_prefix_tokens = std::min(intent.prefix_tokens, intent.seq_tokens);
    const auto pages_after = ceil_div(intent.seq_tokens, intent.page_size);
    const auto pages_before = ceil_div(bounded_prefix_tokens, intent.page_size);
    return pages_after > pages_before ? pages_after - pages_before : 0;
}

/**
 * @brief Builds a complete extend-allocation intent from token counts.
 */
inline ExtendAllocationIntent make_extend_allocation_intent(const ExtendAllocationInput & input) {
    auto intent = ExtendAllocationIntent{
        .batch_size = input.batch_size == 0 ? uint64_t{ 1 } : input.batch_size,
        .page_size = input.page_size,
        .seq_tokens = input.token_count,
    };
    if (input.token_count == 0 || input.page_size == 0) return intent;

    intent.prefix_tokens = std::min(input.token_count, input.prefix_tokens);
    intent.extend_tokens = input.token_count - intent.prefix_tokens;
    intent.requested_tokens = extend_requested_tokens(ExtendPressureInput{
        .extend_tokens = intent.extend_tokens,
        .batch_size = intent.batch_size,
        .page_size = input.page_size,
    });
    intent.requested_pages = ceil_div(intent.requested_tokens, input.page_size);
    intent.allocated_pages = extend_allocated_pages(intent);
    return intent;
}

/**
 * @brief Computes pages that must be cleaned under SGLang allocator semantics.
 *
 * SGLang invokes radix eviction only when allocator availability is insufficient.
 * Once triggered, the cleanup budget is the full allocation request rather than
 * only the free-space deficit. Without a request, only current excess is removed.
 */
inline uint64_t allocation_cleanup_target(const HostCleanupInput & input) {
    if (input.capacity_pages == 0) return 0;

    const auto committed_pages = core::checked_add_u64(input.occupied_pages, input.reserved_pages, "HiCache committed host pages exceed uint64 range");
    const auto excess_pages = committed_pages > input.capacity_pages ? committed_pages - input.capacity_pages : 0;
    if (input.requested_pages == 0) return excess_pages;

    const auto free_pages = committed_pages < input.capacity_pages ? input.capacity_pages - committed_pages : 0;
    if (free_pages >= input.requested_pages) return excess_pages;
    return std::max(excess_pages, input.requested_pages);
}

/** @brief Performs saturating subtraction for unsigned counters. */
inline uint64_t bounded_subtract(uint64_t value, uint64_t decrement) { return decrement >= value ? 0 : value - decrement; }

/** @brief Builds an operation header with stable source-fact provenance. */
inline HiCacheOperationHeader make_operation_header(HiCacheOperationKind kind, const std::string & operation_id, const HiCacheFact & fact,
                                                    const std::string & cache_scope, const std::string & request_key, const std::string & owner,
                                                    const std::vector<std::string> & pages, uint64_t enqueue_epoch) {
    return HiCacheOperationHeader{
        .operation_id = operation_id,
        .kind = kind,
        .cache_scope = cache_scope,
        .request_key = request_key,
        .request_id = fact.request_id,
        .owner = owner,
        .pages = pages,
        .enqueue_epoch = enqueue_epoch,
        .enqueue_ts = fact.ts,
        .source_node_id = fact.source_node_id,
        .source_event_index = fact.source_event_index,
        .source_fact_seq_no = fact.seq_no,
        .source_fact_role = fact.role,
        .source_token_path_id = fact.full_path_span.path_id,
        .source_token_begin = fact.full_path_span.begin,
        .source_token_end = fact.full_path_span.end,
    };
}

} // namespace detail

using detail::allocation_cleanup_target;
using detail::bounded_subtract;
using detail::ceil_div;
using detail::flatten_node_pages;
using detail::has_host_backup;
using detail::HostCleanupInput;
using detail::make_extend_allocation_intent;
using detail::make_operation_header;
using detail::prefetch_active;
using detail::prefix_to;
using detail::suffix_from;

} // namespace markov::trace_graph::modules::hicache::model
