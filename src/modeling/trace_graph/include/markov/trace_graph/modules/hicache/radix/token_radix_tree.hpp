/**
 * @file
 * @brief Canonical HiCache token radix tree and page-residency source of truth.
 */
#pragma once


#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::radix {


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

/**
 * @brief Result of deleting an unbacked device leaf under write-through rules.
 *
 * SGLang removes an unbacked device leaf from the active radix topology instead
 * of leaving a residency-less node (and its hit count) behind for a later request.
 */
struct HiCacheDeviceEvictionResult {
    bool evicted = false;
    HiCacheNodeId node_id = 0;
    HiCacheNodeId parent_node = 0;
    std::vector<std::string> pages;
    std::vector<HiCacheNodeId> affected_nodes;
    std::string reason;
};


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

    /** @brief Removes an unbacked device leaf rather than preserving a ghost node. */
    [[nodiscard]] HiCacheDeviceEvictionResult evict_unbacked_device_leaf(HiCacheNodeId node_id);

private:
    /** @brief Topology coordinates for splitting one compressed radix child. */
    struct ChildSplitRequest {
        HiCacheNodeId parent = 0;
        HiCacheNodeId child = 0;
        size_t split_pages = 0;
    };

    std::vector<HiCacheCacheNode> nodes_;
    std::unordered_map<std::string, HiCacheNodeId> page_to_node_;
    uint64_t access_clock_ = 0;

    [[nodiscard]] HiCacheNodeId create_child(HiCacheNodeId parent, std::vector<std::string> pages);
    [[nodiscard]] HiCacheNodeId insert_suffix(HiCacheNodeId parent, const std::vector<std::string> & pages, size_t offset);
    [[nodiscard]] HiCacheNodeId split_child(const ChildSplitRequest & request);
    [[nodiscard]] HiCachePathLookup lookup_impl(const std::vector<std::string> & pages, bool refresh_access);
    [[nodiscard]] bool has_backup_child(HiCacheNodeId node_id) const;
    void deactivate_subtree(HiCacheNodeId node_id, std::vector<HiCacheNodeId> & affected_nodes);
    void touch_node(HiCacheNodeId node_id);
};

} // namespace markov::trace_graph::modules::hicache::radix
