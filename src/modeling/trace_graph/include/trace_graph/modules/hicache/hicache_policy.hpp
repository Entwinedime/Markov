#pragma once

#include "trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief HiCache target policy 的解析结果。
 *
 * 该结构把 target config、SGLang 默认值和模型约束展开成一份可审计策略。状态机
 * 只读取 resolved 字段，避免在不同 handler 中重复推导默认值。
 */
struct HiCacheResolvedPolicyState {
    uint64_t page_size = 0;
    std::string page_size_source;
    uint64_t l1_capacity_pages = 0;
    std::string l1_capacity_source;
    uint64_t l2_capacity_pages = 0;
    std::string l2_capacity_source;
    std::string write_policy;
    std::string write_policy_source;
    uint64_t write_through_threshold = 0;
    std::string write_through_threshold_source;
    bool write_count_enabled = false;
    bool write_back_enabled = false;
    std::string prefetch_policy;
    std::string prefetch_policy_source;
    uint64_t prefetch_threshold_tokens = 0;
    uint64_t prefetch_threshold_pages = 0;
    std::string prefetch_threshold_source;
    uint64_t prefetch_capacity_limit_pages = 0;
    std::string prefetch_capacity_limit_source;
    std::string host_cleanup_budget_rule;
    std::string host_cleanup_budget_source;
    uint64_t extend_allocation_batch_size = 1;
    std::string extend_allocation_batch_source;
    std::string extend_allocation_rule;
    bool device_allocator_need_sort = false;
    std::string device_allocator_need_sort_source;
    std::string storage_hit_policy;
    std::string storage_hit_policy_source;
    bool prefetch_timeout_configured = false;
    double prefetch_timeout_base_sec = 0.0;
    double prefetch_timeout_per_ki_token_sec = 0.0;
    double prefetch_timeout_max_sec = 0.0;
    std::string prefetch_timeout_source;
    std::string prefetch_timeout_rule;
    std::string prefetch_rate_limit_rule;
    std::vector<std::string> terminal_prefetch_checkpoint_kinds;
    std::vector<std::string> resolution_notes;
};

/**
 * @brief 单次 runtime policy 分支的审计记录。
 *
 * resolved policy 解释静态配置来源；该记录解释某个 request/checkpoint 在当时的
 * target state 下为什么被接受、拒绝、等待或截断。
 */
struct HiCachePolicyDecisionRecord {
    uint64_t decision_epoch = 0;
    std::string cache_scope;
    std::string request_key;
    std::string operation_id;
    std::string role;
    std::string event_name;
    std::string policy_area;
    std::string policy_name;
    std::string decision;
    std::string reason;
    bool accepted = false;
    uint64_t requested_pages = 0;
    uint64_t requested_tokens = 0;
    uint64_t candidate_pages = 0;
    uint64_t hit_pages = 0;
    uint64_t hit_count = 0;
    uint64_t batch_size = 0;
    uint64_t extend_tokens = 0;
    uint64_t allocated_pages = 0;
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
    std::vector<std::string> pages;
};

/**
 * @brief HiCache target policy 的只读决策层。
 *
 * policy 只读 target config 和调用方传入的 state 计数，不直接修改 radix tree。
 */
class HiCachePolicy {
public:
    explicit HiCachePolicy(HiCacheConfig config = HiCacheConfig{});

    [[nodiscard]] const HiCacheResolvedPolicyState & resolved() const { return resolved_; }
    [[nodiscard]] uint64_t write_through_threshold() const;
    [[nodiscard]] bool write_count_enabled() const;
    [[nodiscard]] bool write_back_enabled() const;

    [[nodiscard]] uint64_t prefetch_threshold_pages() const;
    [[nodiscard]] uint64_t prefetch_capacity_limit_pages() const;
    [[nodiscard]] bool prefetch_rate_limited(uint64_t active_requested_pages) const;
    [[nodiscard]] bool terminal_prefetch_checkpoint(const std::string & check_kind) const;
    [[nodiscard]] bool prefetch_timeout_elapsed(uint64_t enqueue_ts, uint64_t checkpoint_ts, uint64_t token_count) const;
    [[nodiscard]] uint64_t extend_allocation_batch_size() const { return resolved_.extend_allocation_batch_size; }

    [[nodiscard]] uint64_t l1_capacity_pages() const { return resolved_.l1_capacity_pages; }
    [[nodiscard]] uint64_t l2_capacity_pages() const { return resolved_.l2_capacity_pages; }
    [[nodiscard]] const std::string & write_policy() const { return resolved_.write_policy; }
    [[nodiscard]] const std::string & prefetch_policy() const { return resolved_.prefetch_policy; }

private:
    HiCacheResolvedPolicyState resolved_;
};

/** @brief 将 HiCacheConfig 展开成状态机使用的 target policy。 */
[[nodiscard]] HiCacheResolvedPolicyState resolve_hicache_policy(const HiCacheConfig & config);

} // namespace TraceGraph
