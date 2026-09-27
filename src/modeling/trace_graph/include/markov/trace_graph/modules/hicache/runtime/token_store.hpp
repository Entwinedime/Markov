/**
 * @file
 * @brief Fact-local HiCache token-path resolution.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <cstdint>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Status returned by a role-specific fact-local resolver.
 */
enum class HiCacheTokenResolutionStatus : std::uint8_t {
    Direct,
    Missing,
    WrongStageRejected,
    SourceClassRejected,
};



/**
 * @brief Result of resolving one scalar fact-local token path.
 */
struct HiCacheTokenResolution {
    HiCacheTokenResolutionStatus status = HiCacheTokenResolutionStatus::Missing;
    HiCacheTokenPath tokens;
    uint64_t token_count = 0;

    /** @brief Returns whether the state model may consume this result. */
    [[nodiscard]] bool ok() const { return status == HiCacheTokenResolutionStatus::Direct; }
};

/**
 * @brief Result of resolving a batch-level `cache_extend_input` fact.
 */
struct HiCacheBatchTokenResolution {
    HiCacheTokenResolutionStatus status = HiCacheTokenResolutionStatus::Missing;
    std::vector<HiCacheTokenResolution> entries;

    /** @brief Returns whether every batch entry is directly consumable. */
    [[nodiscard]] bool ok() const { return status == HiCacheTokenResolutionStatus::Direct; }
};

// Resolution is stateless and owns its selected tokens; no other request or stage supplies a fallback.

/** @brief Resolves a cache-lookup path strictly from the current fact. */
[[nodiscard]] HiCacheTokenResolution resolve_cache_lookup_path(const HiCacheFact & fact);

/** @brief Resolves batch cache-extend paths strictly from the current fact. */
[[nodiscard]] HiCacheBatchTokenResolution resolve_cache_extend_paths(const HiCacheFact & fact);

/** @brief Resolves a finished/unfinished lifecycle path from the current fact. */
[[nodiscard]] HiCacheTokenResolution resolve_cache_lifecycle_commit_path(const HiCacheFact & fact);

/** @brief Resolves a speculative prefetch candidate without committing it. */
[[nodiscard]] HiCacheTokenResolution resolve_prefetch_candidate_path(const HiCacheFact & fact);

} // namespace markov::trace_graph::modules::hicache::runtime
