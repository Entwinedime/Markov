/**
 * @file
 * @brief Incremental HiCache host/device capacity index.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

using radix::HiCacheCacheNode;
using radix::HiCacheNodeId;
using radix::HiCacheNodeRefState;
using radix::HiCacheNodeResidency;
using radix::HiCacheTokenRadixTree;

/**
 * @brief Minimal projection needed to account and rank one radix node.
 *
 * The canonical radix node remains the residency and reference source of truth. This
 * record caches only fields needed by capacity accounting and victim selection;
 * page lists and owner maps remain on the canonical tree.
 */
struct HiCacheCapacityNodeRecord {
    HiCacheNodeId node_id = 0;
    bool active = false;
    uint64_t page_count = 0;
    int64_t priority = 0;
    uint64_t last_access_order = 0;
    bool device_present = false;
    bool host_visible = false;
    uint64_t host_ref_total = 0;
    bool device_evictable = false;
    bool host_evictable = false;
};


/**
 * @brief Constant-size capacity view consumed by allocation policy.
 *
 * Victim identities intentionally do not live in this snapshot. They are already held
 * in ordered sets, and materializing duplicate vectors on each synchronization made a
 * logically constant-time read path scale with the number of evictable leaves.
 */
struct HiCacheCapacitySnapshot {
    uint64_t occupied_device_pages = 0;
    uint64_t occupied_host_pages = 0;
    uint64_t reserved_host_pages = 0;
};

/**
 * @brief Mutation-driven L1/L2 capacity and evictable-leaf index.
 *
 * Insert, split, residency, and reference mutations explicitly synchronize the affected
 * closure. Allocation policy can then read counts and the first ordered victim without
 * rescanning the entire canonical tree.
 */
class HiCacheCapacityIndex {
public:
    /** @brief Synchronizes changed nodes plus ancestors and direct children affecting leaf eligibility. */
    void sync_nodes(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes, uint64_t reserved_host_pages);

    /** @brief Synchronizes only asynchronous host reservation, without observing tree nodes. */
    void sync_reservation(uint64_t reserved_host_pages);

    /** @brief Returns the current constant-size capacity view. */
    [[nodiscard]] const HiCacheCapacitySnapshot & snapshot() const { return snapshot_; }


    /** @brief Returns the first ordered L1 victim, or no value when none is evictable. */
    [[nodiscard]] std::optional<HiCacheNodeId> first_device_victim() const;
    /** Snapshot candidate identities for one eviction call, not a copy of residency. */
    [[nodiscard]] std::vector<HiCacheNodeId> device_victims() const;

    /** @brief Returns the first unreferenced L2 victim, or no value when none is evictable. */
    [[nodiscard]] std::optional<HiCacheNodeId> first_host_victim() const;

private:
    struct VictimKey {
        int64_t priority = 0;
        uint64_t last_access_order = 0;
        HiCacheNodeId node_id = 0;

        [[nodiscard]] bool operator<(const VictimKey & other) const;
    };

    uint64_t occupied_device_pages_ = 0;
    uint64_t occupied_host_pages_ = 0;
    uint64_t reserved_host_pages_ = 0;
    std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> records_;
    std::set<VictimKey> evictable_device_leaves_;
    std::set<VictimKey> evictable_host_leaves_;
    HiCacheCapacitySnapshot snapshot_;

    [[nodiscard]] HiCacheCapacityNodeRecord make_record(const HiCacheTokenRadixTree & tree, HiCacheNodeId node_id) const;
    [[nodiscard]] std::set<HiCacheNodeId> observation_closure(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes) const;
    [[nodiscard]] bool has_device_descendant(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const;
    [[nodiscard]] bool has_backup_child(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const;
    void remove_record_contribution(const HiCacheCapacityNodeRecord & record);
    void add_record_contribution(const HiCacheCapacityNodeRecord & record);
    void update_snapshot();
    [[nodiscard]] static VictimKey victim_key(const HiCacheCapacityNodeRecord & record);
};

} // namespace markov::trace_graph::modules::hicache::runtime
