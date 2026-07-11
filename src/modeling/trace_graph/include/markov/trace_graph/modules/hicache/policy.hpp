/**
 * @file
 * @brief Resolved HiCache target policy and side-effect-free policy gates.
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache {

/**
 * @brief Resolved target policy consumed by the state machine.
 *
 * Core fields are the compact production policy. Debug builds additionally retain source
 * attribution and human-readable rules for summaries, keeping explanatory metadata out
 * of every Release model instance.
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
#ifdef DEBUG
    uint64_t page_size = 0;
    std::string page_size_source;
    std::string l1_capacity_source;
    std::string l2_capacity_source;
    std::string write_policy_source;
    std::string write_through_threshold_source;
    std::string prefetch_policy_source;
    uint64_t prefetch_threshold_tokens = 0;
    std::string prefetch_threshold_source;
    std::string prefetch_capacity_limit_source;
    std::string host_cleanup_budget_rule;
    std::string host_cleanup_budget_source;
    std::string extend_allocation_rule;
    std::string device_allocator_need_sort_source;
    std::string storage_hit_policy;
    std::string storage_hit_policy_source;
    std::string prefetch_timeout_source;
    std::string prefetch_timeout_rule;
    std::string prefetch_rate_limit_rule;
    std::vector<std::string> resolution_notes;
#endif
};

/**
 * @brief Diagnostic record for one runtime policy branch.
 *
 * Resolved policy explains static configuration; this record explains why one request or
 * boundary was accepted, rejected, delayed, or truncated under its current target state.
 */
struct HiCachePolicyDecisionRecord {
    uint64_t decision_epoch = 0;
    std::string cache_scope{};
    std::string request_key{};
    std::string operation_id{};
    std::string role{};
    std::string event_name{};
    std::string policy_area{};
    std::string policy_name{};
    std::string decision{};
    std::string reason{};
    bool accepted = false;
    uint64_t requested_pages = 0;
    uint64_t requested_tokens = 0;
    uint64_t candidate_pages = 0;
    uint64_t hit_pages = 0;
    uint64_t hit_count = 0;
    uint64_t batch_size = 0;
    uint64_t accepted_tokens = 0;
    uint64_t target_device_prefix_tokens = 0;
    uint64_t prior_committed_prefix_tokens = 0;
    uint64_t allocation_prefix_tokens = 0;
    uint64_t extend_tokens = 0;
    uint64_t allocated_pages = 0;
    uint64_t active_request_pages = 0;
    uint64_t active_requested_pages = 0;
    uint64_t capacity_pages = 0;
    uint64_t occupied_pages = 0;
    uint64_t reserved_pages = 0;
    uint64_t allocator_free_pages = 0;
    uint64_t allocator_release_pages = 0;
    uint64_t allocator_available_pages = 0;
    uint64_t allocator_available_before_pages = 0;
    uint64_t allocator_consumed_pages = 0;
    uint64_t allocator_released_pages = 0;
    uint64_t lifecycle_duplicate_pages = 0;
    uint64_t lifecycle_tail_pages = 0;
    uint64_t threshold_pages = 0;
    uint64_t limit_pages = 0;
    std::vector<std::string> pages{};
};

/** @brief Runtime boundary used to evaluate target prefetch timeout. */
struct HiCachePrefetchTimeoutObservation {
    uint64_t enqueue_ts = 0;
    uint64_t boundary_ts = 0;
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

#ifdef DEBUG
    /** @brief Returns resolved policy including Debug source attribution. */
    [[nodiscard]] const HiCacheResolvedPolicyState & resolved() const { return resolved_; }
#endif

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

    /** @brief Returns whether a target boundary exceeds the configured prefetch timeout. */
    [[nodiscard]] bool prefetch_timeout_elapsed(const HiCachePrefetchTimeoutObservation & observation) const;

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
