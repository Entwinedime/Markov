/**
 * @file
 * @brief Incremental HiCache host/device capacity index and Debug diagnostics.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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
 * record caches only fields needed by capacity accounting and victim selection. Large
 * page lists and owner maps are retained exclusively in Debug builds for consistency
 * audits; Release updates therefore do not copy them on every state transition.
 */
struct HiCacheCapacityNodeRecord {
    HiCacheNodeId node_id = 0;
    bool active = false;
    uint64_t page_count = 0;
    int64_t priority = 0;
    uint64_t last_access_order = 0;
    bool device_present = false;
    bool host_visible = false;
    bool storage_readable = false;
    uint64_t host_ref_total = 0;
    bool device_evictable = false;
    bool host_evictable = false;
#ifdef DEBUG
    HiCacheNodeId parent = 0;
    std::vector<std::string> pages;
    HiCacheNodeResidency residency;
    HiCacheNodeRefState refs;
    uint64_t observed_epoch = 0;
#endif
};

#ifdef DEBUG
/**
 * @brief Diagnostic record for one incremental capacity-index synchronization.
 */
struct HiCacheCapacityMutation {
    uint64_t mutation_epoch = 0;
    std::string cache_scope;
    std::string reason;
    uint64_t reserved_host_pages = 0;
    std::vector<HiCacheNodeId> observed_nodes;
    std::vector<HiCacheNodeId> device_leaf_entered;
    std::vector<HiCacheNodeId> device_leaf_left;
    std::vector<HiCacheNodeId> host_leaf_entered;
    std::vector<HiCacheNodeId> host_leaf_left;
};

/**
 * @brief Diagnostic explanation of one capacity-victim selection.
 */
struct HiCacheCapacityVictimChoice {
    uint64_t selection_epoch = 0;
    std::string cache_scope;
    std::string tier;
    std::string reason;
    bool selected = false;
    HiCacheNodeId node_id = 0;
    uint64_t page_count = 0;
    int64_t priority = 0;
    uint64_t last_access_order = 0;
    uint64_t occupied_pages = 0;
    uint64_t reserved_host_pages = 0;
    uint64_t capacity_pages = 0;
    uint64_t requested_pages = 0;
    uint64_t excess_pages = 0;
    std::vector<std::string> pages;
};

/**
 * @brief One consistency discrepancy between the index and canonical radix tree.
 */
struct HiCacheCapacityAuditIssue {
    std::string cache_scope;
    std::string issue;
    std::string tier;
    HiCacheNodeId node_id = 0;
    uint64_t indexed_count = 0;
    uint64_t tree_count = 0;
    std::vector<std::string> pages;
};

/**
 * @brief Full Debug audit result for the capacity index.
 */
struct HiCacheCapacityAudit {
    uint64_t indexed_device_pages = 0;
    uint64_t indexed_host_pages = 0;
    uint64_t indexed_storage_pages = 0;
    uint64_t indexed_reserved_host_pages = 0;
    uint64_t tree_device_pages = 0;
    uint64_t tree_host_pages = 0;
    uint64_t tree_storage_pages = 0;
    uint64_t expected_reserved_host_pages = 0;
    std::vector<HiCacheCapacityAuditIssue> issues;

    /** @brief Returns true when the derived index exactly matches the tree. */
    [[nodiscard]] bool ok() const { return issues.empty(); }
};
#endif

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
    uint64_t readable_storage_pages = 0;
    uint64_t reserved_host_pages = 0;
};

/** @brief Capacity and allocation context attached to one victim selection. */
struct HiCacheVictimRequest {
    uint64_t capacity_pages = 0;
    uint64_t requested_pages = 0;
    std::string_view reason;
};

/**
 * @brief Mutation-driven L1/L2 capacity and evictable-leaf index.
 *
 * Insert, split, residency, and reference mutations explicitly synchronize the affected
 * closure. Allocation policy can then read counts and the first ordered victim without
 * rescanning the entire canonical tree. Audit methods independently derive the expected
 * state in Debug builds and never feed decisions back into the model.
 */
class HiCacheCapacityIndex {
public:
    /** @brief Synchronizes changed nodes plus ancestors and direct children affecting leaf eligibility. */
    void sync_nodes(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes, uint64_t reserved_host_pages, std::string_view reason);

    /** @brief Synchronizes only asynchronous host reservation, without observing tree nodes. */
    void sync_reservation(uint64_t reserved_host_pages, std::string_view reason);

    /** @brief Returns the current constant-size capacity view. */
    [[nodiscard]] const HiCacheCapacitySnapshot & snapshot() const { return snapshot_; }

#ifdef DEBUG
    /** @brief Returns cached node projections for Debug audits. */
    [[nodiscard]] const std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> & node_records() const { return records_; }

    /** @brief Returns the number of capacity-index synchronization epochs. */
    [[nodiscard]] uint64_t mutation_epoch() const { return mutation_epoch_; }

    /** @brief Returns the capacity-leaf mutation trace. */
    [[nodiscard]] const std::vector<HiCacheCapacityMutation> & mutation_trace() const { return mutation_trace_; }

    /** @brief Returns the victim-selection explanation trace. */
    [[nodiscard]] const std::vector<HiCacheCapacityVictimChoice> & victim_choices() const { return victim_choices_; }

    /** @brief Independently checks the incremental index against the canonical tree. */
    [[nodiscard]] HiCacheCapacityAudit audit(const HiCacheTokenRadixTree & tree, uint64_t expected_reserved_host_pages) const;

    /** @brief Returns the number of recorded victim selections. */
    [[nodiscard]] uint64_t victim_selection_count() const { return victim_selection_epoch_; }
#endif

    /** @brief Returns L1 pages beyond configured target capacity. */
    [[nodiscard]] uint64_t device_excess_pages(uint64_t capacity_pages) const;

    /** @brief Returns L2 pages beyond capacity, including active prefetch reservations. */
    [[nodiscard]] uint64_t host_excess_pages(uint64_t capacity_pages) const;

    /** @brief Returns the first ordered L1 victim, or no value when none is evictable. */
    [[nodiscard]] std::optional<HiCacheNodeId> first_device_victim() const;

    /** @brief Returns the first unreferenced L2 victim, or no value when none is evictable. */
    [[nodiscard]] std::optional<HiCacheNodeId> first_host_victim() const;

    /** @brief Selects the current L1 victim and records an explanation in Debug builds. */
    std::optional<HiCacheNodeId> select_device_victim(const HiCacheVictimRequest & request);

    /** @brief Selects the current L2 victim and records an explanation in Debug builds. */
    std::optional<HiCacheNodeId> select_host_victim(const HiCacheVictimRequest & request);

private:
    struct VictimKey {
        int64_t priority = 0;
        uint64_t last_access_order = 0;
        HiCacheNodeId node_id = 0;

        [[nodiscard]] bool operator<(const VictimKey & other) const;
    };

    uint64_t occupied_device_pages_ = 0;
    uint64_t occupied_host_pages_ = 0;
    uint64_t readable_storage_pages_ = 0;
    uint64_t reserved_host_pages_ = 0;
    std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> records_;
    std::set<VictimKey> evictable_device_leaves_;
    std::set<VictimKey> evictable_host_leaves_;
    HiCacheCapacitySnapshot snapshot_;
#ifdef DEBUG
    uint64_t mutation_epoch_ = 0;
    uint64_t victim_selection_epoch_ = 0;
    std::vector<HiCacheCapacityMutation> mutation_trace_;
    std::vector<HiCacheCapacityVictimChoice> victim_choices_;
#endif

    [[nodiscard]] HiCacheCapacityNodeRecord make_record(const HiCacheTokenRadixTree & tree, HiCacheNodeId node_id) const;
    [[nodiscard]] std::set<HiCacheNodeId> observation_closure(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes) const;
    [[nodiscard]] bool has_device_descendant(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const;
    [[nodiscard]] bool has_backup_child(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const;
    void remove_record_contribution(const HiCacheCapacityNodeRecord & record);
    void add_record_contribution(const HiCacheCapacityNodeRecord & record);
#ifdef DEBUG
    [[nodiscard]] const HiCacheCapacityNodeRecord * indexed_record(HiCacheNodeId node_id) const;
    [[nodiscard]] HiCacheCapacityVictimChoice make_victim_choice(std::string_view tier, const HiCacheVictimRequest & request,
                                                                 std::optional<HiCacheNodeId> node_id) const;
    [[nodiscard]] std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> derive_tree_records(const HiCacheTokenRadixTree & tree) const;
#endif
    void update_snapshot();
    [[nodiscard]] static VictimKey victim_key(const HiCacheCapacityNodeRecord & record);
};

} // namespace markov::trace_graph::modules::hicache::runtime
