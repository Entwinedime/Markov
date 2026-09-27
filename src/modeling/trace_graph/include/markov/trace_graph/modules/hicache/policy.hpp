/**
 * @file
 * @brief Resolved HiCache target policy and side-effect-free policy gates.
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace markov::trace_graph::modules::hicache {

/**
 * @brief Resolved target policy consumed by the state machine.
 */
struct HiCacheResolvedPolicyState {
    uint64_t l1_capacity_pages = 0;
    uint64_t l2_capacity_pages = 0;
    std::string write_policy;
    uint64_t write_through_threshold = 0;
    bool write_count_enabled = false;
    bool write_back_enabled = false;
    std::string prefetch_policy;
    uint64_t prefetch_threshold_pages = 0;
    uint64_t prefetch_capacity_limit_pages = 0;
    bool device_allocator_need_sort = false;
    bool prefetch_timeout_configured = false;
    double prefetch_timeout_base_sec = 0.0;
    double prefetch_timeout_per_ki_token_sec = 0.0;
    double prefetch_timeout_max_sec = 0.0;
};

/** @brief Target-derived inputs used to compute the configured prefetch deadline. */
struct HiCachePrefetchTimeoutInput {
    uint64_t enqueue_ts = 0;
    uint64_t token_count = 0;
};

/**
 * @brief Read-only decision layer for HiCache target policy.
 *
 * Policy reads resolved target configuration and caller-supplied counters. It never
 * mutates the radix tree or runtime state.
 */
class HiCachePolicy {
public:
    explicit HiCachePolicy(const frontend::HiCacheConfig & config = frontend::HiCacheConfig{});

    /** @brief Returns the request-hit threshold for write-through backup. */
    [[nodiscard]] uint64_t write_through_threshold() const;

    /** @brief Returns whether backup policy depends on hit count. */
    [[nodiscard]] bool write_count_enabled() const;

    /** @brief Returns whether the target policy is write-back. */
    [[nodiscard]] bool write_back_enabled() const;

    /** @brief Returns the minimum page threshold used by prefetch stop policy. */
    [[nodiscard]] uint64_t prefetch_threshold_pages() const;

    /** @brief Returns the active prefetch reservation limit. */
    [[nodiscard]] uint64_t prefetch_capacity_limit_pages() const;

    /** @brief Returns whether active prefetch pages hit the target rate limit. */
    [[nodiscard]] bool prefetch_rate_limited(uint64_t active_requested_pages) const;

    /** @brief Returns the target timeout deadline, or no value when timeout is not configured. */
    [[nodiscard]] std::optional<uint64_t> prefetch_timeout_deadline_ts(const HiCachePrefetchTimeoutInput & input) const;

    /** @brief Returns target L1/device capacity in pages. */
    [[nodiscard]] uint64_t l1_capacity_pages() const { return resolved_.l1_capacity_pages; }

    /** @brief Returns target L2/host capacity in pages. */
    [[nodiscard]] uint64_t l2_capacity_pages() const { return resolved_.l2_capacity_pages; }

    /** @brief Returns the normalized target write-policy name. */
    [[nodiscard]] const std::string & write_policy() const { return resolved_.write_policy; }

    /** @brief Returns the normalized target prefetch-policy name. */
    [[nodiscard]] const std::string & prefetch_policy() const { return resolved_.prefetch_policy; }

    /** @brief Returns whether the target device allocator sorts its release queue. */
    [[nodiscard]] bool device_allocator_need_sort() const { return resolved_.device_allocator_need_sort; }

private:
    HiCacheResolvedPolicyState resolved_;
};

} // namespace markov::trace_graph::modules::hicache
