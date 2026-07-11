/**
 * @file
 * @brief Canonical HiCache token radix tree and page-residency source of truth.
 */
#pragma once

#ifdef DEBUG
#include "markov/trace_graph/modules/hicache/runtime/target_pager.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::radix {

#ifdef DEBUG
using runtime::HiCachePagePath;
using runtime::HiCacheProjectedPage;
#endif

using HiCacheNodeId = size_t;

/**
 * @brief Device, host, and storage residency for one radix node.
 *
 * Final-state page sets must be derived from these fields rather than maintained
 * as a second source of cache state.
 */
struct HiCacheNodeResidency {
    bool device_present = false;
    bool device_dirty = false;
    bool host_present = false;
    bool host_visible = false;
    bool storage_known = false;
    bool storage_readable = false;
};

/**
 * @brief Protection references held on one radix node.
 *
 * Request, write, load, storage, and prefetch lifecycles may hold references
 * concurrently, with independent release by owner identity.
 */
struct HiCacheNodeRefState {
    uint64_t lock_ref_total = 0;
    uint64_t host_ref_total = 0;
    std::map<std::string, uint64_t> lock_refs_by_owner;
    std::map<std::string, uint64_t> host_refs_by_owner;
};

/**
 * @brief Stateful node in the canonical radix tree.
 *
 * `pages` is the compressed target-page group on the incoming radix edge. Residency
 * and references apply to the complete group, preserving SGLang leaf-group eviction
 * and writeback semantics.
 */
struct HiCacheCacheNode {
    HiCacheNodeId id = 0;
    HiCacheNodeId parent = 0;
    std::map<std::string, HiCacheNodeId> children;
    std::vector<std::string> pages;
    int64_t priority = 0;
    uint64_t last_access_order = 0;
    bool active = true;
    HiCacheNodeResidency residency;
    HiCacheNodeRefState refs;
    uint64_t hit_count = 0;
};

/**
 * @brief Node and page-prefix result from one radix lookup.
 */
struct HiCachePathLookup {
    HiCacheNodeId terminal_node = 0;
    HiCacheNodeId deepest_device_node = 0;
    HiCacheNodeId deepest_host_node = 0;
    std::vector<std::string> topology_pages;
    std::vector<std::string> device_pages;
    std::vector<std::string> host_pages;
    std::vector<std::string> storage_pages;
    std::vector<std::string> visible_pages;
    std::vector<HiCacheNodeId> topology_chain;
    std::vector<HiCacheNodeId> device_chain;
    std::vector<HiCacheNodeId> host_chain;
};

/**
 * @brief Structured state changes produced by one path insertion.
 */
struct HiCacheInsertResult {
    HiCacheNodeId terminal_node = 0;
    uint64_t existing_device_prefix_pages = 0;
    uint64_t existing_topology_prefix_pages = 0;
    uint64_t inserted_key_pages = 0;
    uint64_t page_aligned_key_pages = 0;
    std::vector<HiCacheNodeId> touched_nodes;
    std::vector<HiCacheNodeId> new_device_nodes;
    std::vector<HiCacheNodeId> restored_device_nodes;
    /** @brief Nodes that changed from clean to dirty during this device insertion. */
    std::vector<HiCacheNodeId> dirtied_device_nodes;
    std::vector<HiCacheNodeId> new_host_nodes;
};

/**
 * @brief Structured result of SGLang-compatible host-leaf eviction.
 *
 * SGLang removes the subtree from `parent.children` rather than clearing only the
 * host value. The model returns every affected node so capacity and reference
 * projections can synchronize their records.
 */
struct HiCacheHostEvictionResult {
    bool evicted = false;
    HiCacheNodeId node_id = 0;
    HiCacheNodeId parent_node = 0;
    std::vector<std::string> pages;
    std::vector<HiCacheNodeId> affected_nodes;
    std::string reason;
};

#ifdef DEBUG
/** @brief Target projection details retained for a split diagnostics record. */
struct HiCacheNodeSplitProjection {
    uint64_t depth_page_begin = 0;
    uint64_t depth_page_end = 0;
    bool token_span_known = false;
    uint64_t token_begin = 0;
    uint64_t token_end = 0;
    std::vector<std::string> page_hashes;
    std::vector<std::string> storage_keys;
};

/** @brief Debug record describing residency and ref inheritance across a split. */
struct HiCacheNodeSplitRecord {
    std::string cache_scope;
    HiCacheNodeId parent_node = 0;
    HiCacheNodeId prefix_node = 0;
    HiCacheNodeId suffix_node = 0;
    size_t split_pages = 0;
    std::string parent_child_key;
    std::string suffix_child_key;
    std::vector<std::string> prefix_pages;
    std::vector<std::string> suffix_pages;
    HiCacheNodeSplitProjection prefix_projection;
    HiCacheNodeSplitProjection suffix_projection;
    HiCacheNodeResidency prefix_residency;
    HiCacheNodeResidency suffix_residency;
    uint64_t copied_lock_ref_total = 0;
    uint64_t copied_host_ref_total = 0;
    std::vector<std::string> copied_lock_ref_owners;
    std::vector<std::string> copied_host_ref_owners;
    uint64_t inherited_hit_count = 0;
};
#endif

/**
 * @brief Canonical token/page radix tree for one cache scope.
 *
 * Target page paths are radix keys. `HiCacheTargetPager` owns token-to-page
 * projection; this tree owns SGLang `TreeNode`-style residency and references.
 */
class HiCacheTokenRadixTree {
public:
    /** @brief Creates an empty radix tree containing only the topology root. */
    HiCacheTokenRadixTree();

    /** @brief Returns the canonical topology root. */
    [[nodiscard]] const HiCacheCacheNode & root() const { return nodes_.front(); }

    /** @brief Returns the stable node-storage view, including inactive nodes. */
    [[nodiscard]] const std::vector<HiCacheCacheNode> & nodes() const { return nodes_; }

    /** @brief Returns `nullptr` for out-of-range or inactive node IDs. */
    [[nodiscard]] const HiCacheCacheNode * node(HiCacheNodeId node_id) const;

    /** @brief Returns `nullptr` for out-of-range or inactive mutable node IDs. */
    [[nodiscard]] HiCacheCacheNode * mutable_node(HiCacheNodeId node_id);

#ifdef DEBUG
    /** @brief Returns structured radix-split history for validation. */
    [[nodiscard]] const std::vector<HiCacheNodeSplitRecord> & split_history() const { return split_history_; }

    /** @brief Returns the number of radix splits observed by this tree. */
    [[nodiscard]] uint64_t split_count() const { return split_count_; }
#endif

    /** @brief Reports whether a page belongs to the active radix topology. */
    [[nodiscard]] bool contains_page(const std::string & page) const;

    /** @brief Resolves the active node that owns a page. */
    [[nodiscard]] std::optional<HiCacheNodeId> node_for_page(const std::string & page) const;

    /** @brief Returns a node's compressed page group; invalid IDs throw. */
    [[nodiscard]] const std::vector<std::string> & node_pages(HiCacheNodeId node_id) const;

    /** @brief Returns the root-to-terminal ancestor chain, excluding the root. */
    [[nodiscard]] std::vector<HiCacheNodeId> ancestor_node_ids(HiCacheNodeId terminal_node) const;

    /** @brief Flattens compressed groups from root through the terminal node. */
    [[nodiscard]] std::vector<std::string> flattened_pages(HiCacheNodeId terminal_node) const;

#ifdef DEBUG
    /** @brief Retains page projection metadata for split diagnostics. */
    void observe_page_path(const HiCachePagePath & path);
#endif

    /** @brief Looks up a page path and refreshes access order for matched nodes. */
    [[nodiscard]] HiCachePathLookup lookup(const std::vector<std::string> & pages);

    /** @brief Looks up a path without refreshing access order; topology may still split. */
    [[nodiscard]] HiCachePathLookup lookup_peek(const std::vector<std::string> & pages);

    /** @brief Returns the contiguous visible prefix across selected residency tiers. */
    [[nodiscard]] std::vector<std::string> contiguous_prefix(const std::vector<std::string> & pages, bool include_device, bool include_host,
                                                             bool include_storage);

    /** @brief Inserts device residency and applies target priority and dirty state. */
    HiCacheInsertResult insert_device_path(const std::vector<std::string> & pages, int64_t priority, bool dirty);

    /** @brief Materializes a complete page path into host residency. */
    HiCacheInsertResult insert_host_path(const std::vector<std::string> & pages, bool storage_readable);

    /** @brief Materializes only node groups fully covered by `visible_pages`. */
    HiCacheInsertResult insert_host_path(const std::vector<std::string> & pages, const std::set<std::string> & visible_pages, bool storage_readable);

    /** @brief Refreshes access order for capacity-victim selection. */
    void touch_chain(const std::vector<HiCacheNodeId> & chain);

    /** @brief Adds one lock-reference count for an owner along a node chain. */
    void add_lock_ref(const std::vector<HiCacheNodeId> & chain, const std::string & owner);

    /** @brief Adds one host-reference count for an owner along a node chain. */
    void add_host_ref(const std::vector<HiCacheNodeId> & chain, const std::string & owner);

    /** @brief Releases every tree reference held by an owner. */
    void release_refs_by_owner(const std::string & owner);

    /** @brief Marks host visibility and monotonically updates storage readability. */
    void mark_host_visible(HiCacheNodeId node_id, bool storage_readable);

    /** @brief Clears device dirty state for an active node. */
    void clear_dirty(HiCacheNodeId node_id);

    /** @brief Removes device residency, optionally materializing a readable host backup. */
    void demote_device_to_host(HiCacheNodeId node_id, bool ensure_host);

    /** @brief Removes ordinary device residency without changing host or storage state. */
    void remove_device_regular(HiCacheNodeId node_id);

    /** @brief Removes a host leaf or subtree under SGLang eviction semantics. */
    [[nodiscard]] HiCacheHostEvictionResult evict_host_leaf(HiCacheNodeId node_id);

private:
    /** @brief Topology coordinates for splitting one compressed radix child. */
    struct ChildSplitRequest {
        HiCacheNodeId parent = 0;
        HiCacheNodeId child = 0;
        size_t split_pages = 0;
    };

    std::vector<HiCacheCacheNode> nodes_;
#ifdef DEBUG
    std::vector<HiCacheNodeSplitRecord> split_history_;
    std::unordered_map<std::string, HiCacheProjectedPage> page_projection_;
    uint64_t split_count_ = 0;
#endif
    std::unordered_map<std::string, HiCacheNodeId> page_to_node_;
    uint64_t access_clock_ = 0;

    [[nodiscard]] HiCacheNodeId create_child(HiCacheNodeId parent, std::vector<std::string> pages);
    [[nodiscard]] HiCacheNodeId insert_suffix(HiCacheNodeId parent, const std::vector<std::string> & pages, size_t offset);
    [[nodiscard]] HiCacheNodeId split_child(const ChildSplitRequest & request);
#ifdef DEBUG
    [[nodiscard]] HiCacheNodeSplitProjection split_projection(const std::vector<std::string> & pages, uint64_t depth_page_begin) const;
#endif
    [[nodiscard]] HiCachePathLookup lookup_impl(const std::vector<std::string> & pages, bool refresh_access);
    [[nodiscard]] bool has_backup_child(HiCacheNodeId node_id) const;
    void deactivate_subtree(HiCacheNodeId node_id, std::vector<HiCacheNodeId> & affected_nodes);
    void touch_node(HiCacheNodeId node_id);
};

} // namespace markov::trace_graph::modules::hicache::radix
