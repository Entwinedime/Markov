/**
 * @file
 * @brief Strict gate from HiCache facts to state-model roles.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache {

/**
 * @brief Fact roles currently accepted by the C++ HiCache state model.
 *
 * `Unknown` is diagnostic only and must never become a fallback processing branch.
 */
enum class HiCacheFactRole : std::uint8_t {
    Unknown,
    PrefetchCandidateAnchor,
    CacheLookupInput,
    CacheExtendInput,
    CacheLifecycleCommit,
};

/**
 * @brief Result of applying the state-model consumer and role gates.
 *
 * `model_fact` means the catalog declared `hicache_state_model` as a consumer.
 * `known_role` means class, role, and phase belong to the active whitelist. Keeping them
 * separate distinguishes unrelated facts from malformed intended model inputs.
 */
struct HiCacheFactRoute {
    bool model_fact = false;
    bool known_role = false;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
};

/** @brief Parses a role token into the active whitelist enum. */
[[nodiscard]] HiCacheFactRole parse_hicache_fact_role(std::string_view role);

/** @brief Returns the stable artifact name for a role. */
[[nodiscard]] std::string hicache_fact_role_name(HiCacheFactRole role);

/** @brief Applies consumer, phase, class, and role gates to one fact. */
[[nodiscard]] HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact);

/**
 * @brief Returns missing required fields for one approved role.
 *
 * Missing fields become explicit contract errors; source outcomes are never used as fallback.
 */
[[nodiscard]] std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role);

} // namespace markov::trace_graph::modules::hicache
