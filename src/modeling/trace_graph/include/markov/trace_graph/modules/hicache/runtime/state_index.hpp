/**
 * @file
 * @brief Debug-only final-state views derived from canonical HiCache runtime structures.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"
#include "markov/trace_graph/modules/hicache/storage/storage_directory.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

using radix::HiCacheTokenRadixTree;
using storage::HiCacheStorageDirectory;

/**
 * @brief Selects the residency scope of a derived final-state view.
 */
enum class HiCacheDerivedStateMode : std::uint8_t { MaterializedOnly, StorageDirectoryInclusive };

/** @brief Returns the stable artifact name for a derivation mode. */
[[nodiscard]] std::string hicache_derived_state_mode_name(HiCacheDerivedStateMode mode);

/**
 * @brief Immutable derived HiCache final-state snapshot.
 *
 * These sets are not state sources. They are collected from canonical radix residency and
 * references, asynchronous operations, and optionally the storage directory for diagnostics.
 */
struct HiCacheDerivedStateSnapshot {
    HiCacheDerivedStateMode mode = HiCacheDerivedStateMode::MaterializedOnly;
    std::set<std::string> l1;
    std::set<std::string> l2;
    std::set<std::string> l3;
    std::set<std::string> dirty;
    std::set<std::string> backuped;
    std::set<std::string> evicted;
    std::set<std::string> locked;
    std::set<std::string> pending_writeback;
    std::set<std::string> prefetch_planned;
    std::set<std::string> prefetch_ready;
    std::set<std::string> prefetch_late;
    std::set<std::string> prefetch_suppressed;
    std::map<std::string, uint64_t> page_hit_counts;

    /** @brief Builds a deterministic digest of every field in this snapshot. */
    [[nodiscard]] std::string digest() const;
};

/**
 * @brief Read-only accumulator for a derived state snapshot.
 */
class HiCacheDerivedStateView {
public:
    explicit HiCacheDerivedStateView(HiCacheDerivedStateMode mode = HiCacheDerivedStateMode::MaterializedOnly);

    /** @brief Includes materialized residency, references, and hit counts from the radix tree. */
    void include_tree(const HiCacheTokenRadixTree & tree);

    /** @brief Includes prefetch and writeback lifecycle projections from operation tables. */
    void include_async(const HiCacheAsyncOperationTable & async_ops);

    /** @brief Includes backend-readable storage identities only in inclusive mode. */
    void include_storage_directory(const HiCacheStorageDirectory & storage);

    /** @brief Returns the accumulated snapshot by value. */
    [[nodiscard]] HiCacheDerivedStateSnapshot snapshot() const { return snapshot_; }

private:
    HiCacheDerivedStateSnapshot snapshot_;
};

/** @brief Copies an ordered set into a stable vector for JSON artifacts. */
[[nodiscard]] std::vector<std::string> hicache_sorted_vector(const std::set<std::string> & values);

} // namespace markov::trace_graph::modules::hicache::runtime
