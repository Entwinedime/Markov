/**
 * @file
 * @brief Fact-local HiCache token-path resolution and Debug request history.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <cstdint>
#include <string>
#ifdef DEBUG
#include <cstddef>
#include <unordered_map>
#endif
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Completeness of a Debug token-path snapshot.
 *
 * This distinguishes missing input from a path shorter than one target page. It supports
 * diagnostics only and never supplies fallback tokens to the state model.
 */
#ifdef DEBUG
enum class HiCacheTokenCompleteness : std::uint8_t { Unknown, Partial, PageAligned, Full };

/**
 * @brief Semantic stage of a Debug token-path snapshot.
 *
 * One SGLang request forms a growing token timeline. Explicit stages prevent speculative
 * prefetch candidates from being confused with committed cache-extend/lifecycle paths.
 */
enum class HiCacheTokenSnapshotStage : std::uint8_t {
    Unknown,
    CacheLookup,
    CacheExtend,
    LifecycleUnfinished,
    LifecycleFinished,
    PrefetchCandidate,
};
#endif

/**
 * @brief Status returned by a role-specific fact-local resolver.
 */
enum class HiCacheTokenResolutionStatus : std::uint8_t {
    Direct,
    Missing,
    WrongStageRejected,
    SourceClassRejected,
};

#ifdef DEBUG
/** @brief Returns a stable diagnostic name for a resolution status. */
[[nodiscard]] std::string hicache_token_resolution_status_name(HiCacheTokenResolutionStatus status);
#endif

#ifdef DEBUG
/**
 * @brief Immutable Debug snapshot of the path explicitly carried by one fact.
 *
 * The directory records this evidence for diagnostics only. It does not evaluate policy
 * or infer whether insertion, prefetch, or backup should occur.
 */
struct HiCacheTokenPathSnapshot {
    size_t source_event_index = 0;
    uint64_t seq_no = 0;
    uint64_t ts = 0;
    HiCacheTokenSnapshotStage stage = HiCacheTokenSnapshotStage::Unknown;
    uint64_t page_aligned_token_count = 0;
    HiCacheTokenCompleteness completeness = HiCacheTokenCompleteness::Unknown;
};
#endif

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
#ifdef DEBUG
    /** @brief Returns a scope/request key, or an empty string without a request ID. */
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;

    /** @brief Records the path explicitly carried by the current fact for diagnostics. */
    void observe_fact_path(const HiCacheFact & fact, uint64_t page_size);
#endif

    /** @brief Resolves a cache-lookup path strictly from the current fact. */
    [[nodiscard]] HiCacheTokenResolution resolve_cache_lookup_path(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief Resolves batch cache-extend paths strictly from the current fact. */
    [[nodiscard]] HiCacheBatchTokenResolution resolve_cache_extend_paths(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief Resolves a finished/unfinished lifecycle path from the current fact. */
    [[nodiscard]] HiCacheTokenResolution resolve_cache_lifecycle_commit_path(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief Resolves a speculative prefetch candidate without committing it. */
    [[nodiscard]] HiCacheTokenResolution resolve_prefetch_candidate_path(const HiCacheFact & fact, uint64_t page_size) const;

#ifdef DEBUG
    /** @brief Returns the most recent committed snapshot preceding the current fact. */
    [[nodiscard]] const HiCacheTokenPathSnapshot * previous_committed_snapshot(const HiCacheFact & fact) const;

private:
    std::vector<HiCacheTokenPathSnapshot> snapshots_;
    std::unordered_map<std::string, std::vector<size_t>> snapshots_by_request_;

    /** @brief Appends a scalar snapshot that already passed the input contract. */
    void append_snapshot(const HiCacheFact & fact, HiCacheTokenSnapshotStage stage, uint64_t page_size);
#endif
};

} // namespace markov::trace_graph::modules::hicache::runtime
