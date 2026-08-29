/**
 * @file
 * @brief Fact-local HiCache token-path resolution and Debug request history.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Completeness of a Debug token-path snapshot.
 *
 * This distinguishes missing input from a path shorter than one target page. It supports
 * diagnostics only and never supplies fallback tokens to the state model.
 */

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
    uint64_t page_aligned_token_count = 0;

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

/**
 * @brief Fact-local token resolver with optional Debug request history.
 *
 * Production resolution is deliberately stateless: every handler consumes only the path
 * present on its current fact. Debug builds additionally retain request timelines for
 * growth diagnostics, but that history is never a fallback source.
 */
class HiCacheTokenDirectory {
public:

    /** @brief Resolves a cache-lookup path strictly from the current fact. */
    [[nodiscard]] HiCacheTokenResolution resolve_cache_lookup_path(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief Resolves batch cache-extend paths strictly from the current fact. */
    [[nodiscard]] HiCacheBatchTokenResolution resolve_cache_extend_paths(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief Resolves a finished/unfinished lifecycle path from the current fact. */
    [[nodiscard]] HiCacheTokenResolution resolve_cache_lifecycle_commit_path(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief Resolves a speculative prefetch candidate without committing it. */
    [[nodiscard]] HiCacheTokenResolution resolve_prefetch_candidate_path(const HiCacheFact & fact, uint64_t page_size) const;

};

} // namespace markov::trace_graph::modules::hicache::runtime
