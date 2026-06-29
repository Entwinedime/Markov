/**
 * @file
 * @brief HiCache target policy 辅助工具实现。
 */
#include "markov/trace_graph/modules/hicache/policy.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace markov::trace_graph::modules::hicache {

using frontend::HiCacheConfig;

namespace policy_detail {

constexpr uint64_t kSglangDefaultPrefetchThresholdTokens = 256;
constexpr uint64_t kSglangWriteThroughThreshold = 1;
constexpr uint64_t kSglangWriteThroughSelectiveThreshold = 2;
constexpr uint64_t kExplicitSingleRequestExtendBatchSize = 1;
constexpr double kSglangPrefetchCapacityRatio = 0.8;

uint64_t ceil_div(uint64_t value, uint64_t divisor) {
    if (divisor == 0) return 0;
    return (value + divisor - 1) / divisor;
}

std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool in_set(std::string_view value, std::initializer_list<std::string_view> allowed) { return std::ranges::find(allowed, value) != allowed.end(); }

uint64_t derived_prefetch_threshold_pages(uint64_t configured_pages, uint64_t page_size, uint64_t & threshold_tokens, std::string & source) {
    /* SGLang 默认 prefetch threshold 以 token 表达；模型内部统一换算成 target
       page 数，使跨 page_size 配置的 policy decision 可比较。 */
    if (configured_pages > 0) {
        threshold_tokens = configured_pages * page_size;
        source = "target_config.prefetch_threshold_pages";
        return configured_pages;
    }
    threshold_tokens = std::max(kSglangDefaultPrefetchThresholdTokens, page_size);
    source = "sglang: max(prefetch_threshold=256 tokens, page_size)";
    return ceil_div(threshold_tokens, page_size);
}

uint64_t derived_prefetch_capacity_limit_pages(const HiCacheConfig & config, std::string & source) {
    /* 这里建模的是 SGLang host side prefetch reservation 上限，而不是 L3 backend
       可读 page 数。limit=0 表示 prefetch 被 rate-limit 完全抑制。 */
    if (config.prefetch_capacity_limit_pages > 0) {
        source = "target_config.prefetch_capacity_limit_pages";
        return config.prefetch_capacity_limit_pages;
    }
    source = "sglang: floor(0.8 * max(l2_capacity_pages - l1_capacity_pages, 0))";
    if (config.l2_capacity_pages <= config.l1_capacity_pages) return 0;
    return static_cast<uint64_t>(static_cast<double>(config.l2_capacity_pages - config.l1_capacity_pages) * kSglangPrefetchCapacityRatio);
}

uint64_t derived_write_threshold(const HiCacheConfig & config, const std::string & write_policy, std::string & source) {
    if (config.write_through_threshold > 0) {
        source = "target_config.write_through_threshold";
        return config.write_through_threshold;
    }
    if (write_policy == "write_through") {
        source = "sglang: write_through threshold = 1";
        return kSglangWriteThroughThreshold;
    }
    if (write_policy == "write_through_selective") {
        source = "sglang: write_through_selective threshold = 2";
        return kSglangWriteThroughSelectiveThreshold;
    }
    source = "write_back: hit-count backup disabled";
    return 0;
}

} // namespace policy_detail

using policy_detail::derived_prefetch_capacity_limit_pages;
using policy_detail::derived_prefetch_threshold_pages;
using policy_detail::derived_write_threshold;
using policy_detail::in_set;
using policy_detail::kExplicitSingleRequestExtendBatchSize;
using policy_detail::lower_copy;

HiCacheResolvedPolicyState resolve_hicache_policy(const HiCacheConfig & config) {
    /* policy resolution 是 target config 的纯函数。它不能读取 source trace 中观测到的
       timeout、hit count 或 cleanup 结果；这些都必须由状态机从输入 fact 推导。 */
    const auto page_size = config.page_size == 0 ? uint64_t{ 1 } : config.page_size;
    const auto write_policy = lower_copy(config.write_policy.empty() ? std::string{ "write_through" } : config.write_policy);
    const auto prefetch_policy = lower_copy(config.prefetch_policy.empty() ? std::string{ "timeout" } : config.prefetch_policy);
    if (!in_set(write_policy, { "write_through", "write_through_selective", "write_back" }))
        throw std::runtime_error("Invalid hicache.write_policy after policy resolution: " + write_policy);
    if (!in_set(prefetch_policy, { "wait_complete", "best_effort", "timeout" }))
        throw std::runtime_error("Invalid hicache.prefetch_policy after policy resolution: " + prefetch_policy);

    uint64_t prefetch_threshold_tokens = 0;
    std::string prefetch_threshold_source;
    const auto prefetch_threshold_pages =
        derived_prefetch_threshold_pages(config.prefetch_threshold_pages, page_size, prefetch_threshold_tokens, prefetch_threshold_source);
    std::string prefetch_capacity_limit_source;
    const auto prefetch_capacity_limit_pages = derived_prefetch_capacity_limit_pages(config, prefetch_capacity_limit_source);
    std::string write_through_threshold_source;
    const auto write_through_threshold = derived_write_threshold(config, write_policy, write_through_threshold_source);

    HiCacheResolvedPolicyState resolved{
        .page_size = page_size,
        .page_size_source = config.page_size > 0 ? "target_config.page_size" : "fallback: 1 page per token for policy projection only",
        .l1_capacity_pages = config.l1_capacity_pages,
        .l1_capacity_source = "target_config.l1_capacity_pages",
        .l2_capacity_pages = config.l2_capacity_pages,
        .l2_capacity_source = "target_config.l2_capacity_pages",
        .write_policy = write_policy,
        .write_policy_source = "target_config.write_policy or ModelConfig default",
        .write_through_threshold = write_through_threshold,
        .write_through_threshold_source = write_through_threshold_source,
        .write_count_enabled = write_policy == "write_through" || write_policy == "write_through_selective",
        .write_back_enabled = write_policy == "write_back",
        .prefetch_policy = prefetch_policy,
        .prefetch_policy_source = "target_config.prefetch_policy or ModelConfig default",
        .prefetch_threshold_tokens = prefetch_threshold_tokens,
        .prefetch_threshold_pages = prefetch_threshold_pages,
        .prefetch_threshold_source = prefetch_threshold_source,
        .prefetch_capacity_limit_pages = prefetch_capacity_limit_pages,
        .prefetch_capacity_limit_source = prefetch_capacity_limit_source,
        .host_cleanup_budget_rule = "current_target_request_pages",
        .host_cleanup_budget_source = "sglang: cleanup budget follows current page-aligned target request",
        .extend_allocation_batch_size = kExplicitSingleRequestExtendBatchSize,
        .extend_allocation_batch_source = "temporary model contract: explicit single-request ScheduleBatch until batch-level state-model fact is collected",
        .extend_allocation_rule = "sglang paged extend pressure: extend_num_tokens + batch_size * page_size; page_size=1 uses extend_num_tokens",
        .device_allocator_need_sort = config.device_allocator_need_sort,
        .device_allocator_need_sort_source = "target_config.device_allocator_need_sort or derived from target_config.disaggregation_mode",
        .storage_hit_policy = "continuous_prefix",
        .storage_hit_policy_source = "sglang: storage hit query keeps only contiguous hit prefix",
        .prefetch_timeout_configured = config.prefetch_timeout_configured,
        .prefetch_timeout_base_sec = config.prefetch_timeout_base_sec,
        .prefetch_timeout_per_ki_token_sec = config.prefetch_timeout_per_ki_token_sec,
        .prefetch_timeout_max_sec = config.prefetch_timeout_max_sec,
        .prefetch_timeout_source = config.prefetch_timeout_configured ? "target_config.prefetch_timeout_*" : "not configured in target config",
        .prefetch_timeout_rule = "min(max, base + per_ki_token * token_count / 1024)",
        .prefetch_rate_limit_rule = "active_requested_pages >= prefetch_capacity_limit_pages",
    };

    if (config.page_size == 0) resolved.resolution_notes.push_back("policy projection used fallback page_size=1 because target_config.page_size is missing");
    if (!config.prefetch_timeout_configured && prefetch_policy == "timeout")
        resolved.resolution_notes.push_back("timeout policy is selected, but modeled timeout requires explicit target timeout config");
    if (prefetch_capacity_limit_pages == 0)
        resolved.resolution_notes.push_back("prefetch capacity limit is zero, so SGLang-style rate limit suppresses storage prefetch");

    return resolved;
}

HiCachePolicy::HiCachePolicy(HiCacheConfig config) : resolved_(resolve_hicache_policy(config)) {}

uint64_t HiCachePolicy::write_through_threshold() const { return resolved_.write_through_threshold; }

bool HiCachePolicy::write_count_enabled() const { return resolved_.write_count_enabled; }

bool HiCachePolicy::write_back_enabled() const { return resolved_.write_back_enabled; }

uint64_t HiCachePolicy::prefetch_threshold_pages() const { return resolved_.prefetch_threshold_pages; }

uint64_t HiCachePolicy::prefetch_capacity_limit_pages() const { return resolved_.prefetch_capacity_limit_pages; }

bool HiCachePolicy::prefetch_rate_limited(uint64_t active_requested_pages) const {
    const auto limit = prefetch_capacity_limit_pages();
    if (limit == 0) return true;
    return active_requested_pages >= limit;
}

bool HiCachePolicy::prefetch_timeout_elapsed(uint64_t enqueue_ts, uint64_t checkpoint_ts, uint64_t token_count) const {
    if (!resolved_.prefetch_timeout_configured || checkpoint_ts <= enqueue_ts) return false;
    const double timeout_sec =
        std::min(resolved_.prefetch_timeout_max_sec,
                 resolved_.prefetch_timeout_base_sec + resolved_.prefetch_timeout_per_ki_token_sec * static_cast<double>(token_count) / 1024.0);
    const auto elapsed_sec = static_cast<double>(checkpoint_ts - enqueue_ts) / 1'000'000.0;
    return elapsed_sec > timeout_sec;
}

} // namespace markov::trace_graph::modules::hicache
