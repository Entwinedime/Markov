/**
 * @file
 * @brief HiCache target-policy resolution and gate implementation.
 */
#include "markov/trace_graph/modules/hicache/policy.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

uint64_t ceil_div(uint64_t value, uint64_t divisor) {
    if (divisor == 0) return 0;
    return value / divisor + static_cast<uint64_t>(value % divisor != 0);
}

std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool in_set(std::string_view value, std::initializer_list<std::string_view> allowed) { return std::ranges::find(allowed, value) != allowed.end(); }

struct PrefetchThresholdInput {
    uint64_t configured_pages = 0;
    uint64_t page_size = 0;
};

uint64_t derived_prefetch_threshold_pages(const PrefetchThresholdInput & input) {
    /**
     * @brief Converts SGLang's token-based default prefetch threshold to pages.
     *
     * Keeping policy in target pages makes decisions consistent across page-size configs.
     */
    if (input.configured_pages > 0) return input.configured_pages;
    return ceil_div(std::max(kSglangDefaultPrefetchThresholdTokens, input.page_size), input.page_size);
}

uint64_t derived_prefetch_capacity_limit_pages(const HiCacheConfig & config) {
    /**
     * @brief Derives SGLang's host-side prefetch reservation limit.
     *
     * This is not L3 readability. A zero limit means rate limiting suppresses prefetch.
     */
    if (config.prefetch_capacity_limit_pages > 0) return config.prefetch_capacity_limit_pages;
    if (config.l2_capacity_pages <= config.l1_capacity_pages) return 0;
    constexpr uint64_t kCapacityRatioNumerator = 4;
    constexpr uint64_t kCapacityRatioDenominator = 5;
    const auto available_pages = config.l2_capacity_pages - config.l1_capacity_pages;
    return (available_pages / kCapacityRatioDenominator) * kCapacityRatioNumerator
           + ((available_pages % kCapacityRatioDenominator) * kCapacityRatioNumerator) / kCapacityRatioDenominator;
}

uint64_t derived_write_threshold(const HiCacheConfig & config, const std::string & write_policy) {
    if (config.write_through_threshold > 0) return config.write_through_threshold;
    if (write_policy == "write_through") return kSglangWriteThroughThreshold;
    if (write_policy == "write_through_selective") return kSglangWriteThroughSelectiveThreshold;
    return 0;
}

} // namespace policy_detail

using policy_detail::derived_prefetch_capacity_limit_pages;
using policy_detail::derived_prefetch_threshold_pages;
using policy_detail::derived_write_threshold;
using policy_detail::in_set;
using policy_detail::lower_copy;
using policy_detail::PrefetchThresholdInput;

HiCacheResolvedPolicyState resolve_hicache_policy(const HiCacheConfig & config) {
    /**
     * @brief Resolves policy as a pure function of target configuration.
     *
     * Source-observed timeout, hit count, and cleanup outcomes cannot enter this boundary;
     * the state machine derives them from approved facts.
     */
    const auto page_size = config.page_size == 0 ? uint64_t{ 1 } : config.page_size;
    const auto write_policy = lower_copy(config.write_policy.empty() ? std::string{ "write_through" } : config.write_policy);
    const auto prefetch_policy = lower_copy(config.prefetch_policy.empty() ? std::string{ "timeout" } : config.prefetch_policy);
    if (!in_set(write_policy, { "write_through", "write_through_selective", "write_back" }))
        throw std::runtime_error("Invalid hicache.write_policy after policy resolution: " + write_policy);
    if (!in_set(prefetch_policy, { "wait_complete", "best_effort", "timeout" }))
        throw std::runtime_error("Invalid hicache.prefetch_policy after policy resolution: " + prefetch_policy);

    const auto prefetch_threshold_pages = derived_prefetch_threshold_pages(PrefetchThresholdInput{
        .configured_pages = config.prefetch_threshold_pages,
        .page_size = page_size,
    });
    const auto prefetch_capacity_limit_pages = derived_prefetch_capacity_limit_pages(config);
    const auto write_through_threshold = derived_write_threshold(config, write_policy);

    return HiCacheResolvedPolicyState{
        .l1_capacity_pages = config.l1_capacity_pages,
        .l2_capacity_pages = config.l2_capacity_pages,
        .write_policy = write_policy,
        .write_through_threshold = write_through_threshold,
        .write_count_enabled = write_policy == "write_through" || write_policy == "write_through_selective",
        .write_back_enabled = write_policy == "write_back",
        .prefetch_policy = prefetch_policy,
        .prefetch_threshold_pages = prefetch_threshold_pages,
        .prefetch_capacity_limit_pages = prefetch_capacity_limit_pages,
        .device_allocator_need_sort = config.device_allocator_need_sort,
        .prefetch_timeout_configured = config.prefetch_timeout_configured,
        .prefetch_timeout_base_sec = config.prefetch_timeout_base_sec,
        .prefetch_timeout_per_ki_token_sec = config.prefetch_timeout_per_ki_token_sec,
        .prefetch_timeout_max_sec = config.prefetch_timeout_max_sec,
    };
}

HiCachePolicy::HiCachePolicy(const HiCacheConfig & config) : resolved_(resolve_hicache_policy(config)) {}

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

std::optional<uint64_t> HiCachePolicy::prefetch_timeout_deadline_ts(const HiCachePrefetchTimeoutInput & input) const {
    if (!resolved_.prefetch_timeout_configured) return std::nullopt;
    const double timeout_sec =
        std::min(resolved_.prefetch_timeout_max_sec,
                 resolved_.prefetch_timeout_base_sec + resolved_.prefetch_timeout_per_ki_token_sec * static_cast<double>(input.token_count) / 1024.0);
    const auto timeout_us = core::truncate_to_u64(std::ceil(timeout_sec * 1'000'000.0));
    if (!timeout_us) throw std::overflow_error("HiCache prefetch timeout exceeds uint64 microsecond range");
    return core::checked_add_u64(input.enqueue_ts, *timeout_us, "HiCache prefetch timeout deadline exceeds uint64 range");
}

} // namespace markov::trace_graph::modules::hicache
