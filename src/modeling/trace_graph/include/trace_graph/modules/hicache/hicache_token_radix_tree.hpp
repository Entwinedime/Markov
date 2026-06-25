#pragma once

#include "trace_graph/modules/hicache/hicache_target_pager.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

using HiCacheNodeId = size_t;

/**
 * @brief 单个 radix node 的 device/host/storage residency。
 *
 * final-state page set 必须从这些字段派生，不能成为另一个事实源。
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
 * @brief 单个 radix node 的保护引用。
 *
 * request、write、load、storage 和 prefetch 可以同时持有引用，并能按 owner 独立释放。
 */
struct HiCacheNodeRefState {
    uint64_t lock_ref_total = 0;
    uint64_t host_ref_total = 0;
    std::map<std::string, uint64_t> lock_refs_by_owner;
    std::map<std::string, uint64_t> host_refs_by_owner;
};

/**
 * @brief canonical radix tree 中的状态节点。
 *
 * pages 是压缩边上的 target page group。一个 node 的 residency/ref 对整段 group
 * 生效，从而保留 SGLang leaf group eviction/writeback 语义。
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
 * @brief radix lookup 的 node/page 结果。
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
 * @brief path insertion 的状态变化结果。
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
    /** @brief 本次 device insert 中从 clean 变为 dirty 的 node。 */
    std::vector<HiCacheNodeId> dirtied_device_nodes;
    std::vector<HiCacheNodeId> new_host_nodes;
};

/**
 * @brief SGLang `evict_host()` 删除 host leaf 后的结构化结果。
 *
 * SGLang 释放 host leaf 时会从 parent.children 中移除整棵子树，而不是只清
 * host_value。模型必须显式返回被移除的 node，供 capacity/ref 审计同步旧 record。
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
 * @brief split record 中单个 node group 的 target projection 摘要。
 */
struct HiCacheNodeSplitProjection {
    uint64_t depth_page_begin = 0;
    uint64_t depth_page_end = 0;
    bool token_span_known = false;
    uint64_t token_begin = 0;
    uint64_t token_end = 0;
    std::vector<std::string> page_hashes;
    std::vector<std::string> storage_keys;
};

/**
 * @brief radix node split 的结构化审计记录。
 *
 * SGLang split 会把一个 child 切成 prefix parent 和 suffix child；value/ref/hit
 * 继承语义必须显式记录，后续 transition exactness 才能解释 split 边界。
 */
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

/**
 * @brief 每个 cache_scope 的 canonical token/page radix tree。
 *
 * 当前模型以 target page path 作为 radix key。token path 到 page path 的投影由
 * HiCacheTargetPager 完成；tree 本身维护 SGLang TreeNode 风格的 residency/ref。
 */
class HiCacheTokenRadixTree {
public:
    HiCacheTokenRadixTree();

    [[nodiscard]] const HiCacheCacheNode & root() const { return nodes_.front(); }
    [[nodiscard]] const std::vector<HiCacheCacheNode> & nodes() const { return nodes_; }
    [[nodiscard]] const std::vector<HiCacheNodeSplitRecord> & split_history() const { return split_history_; }
    [[nodiscard]] const HiCacheCacheNode * node(HiCacheNodeId node_id) const;
    [[nodiscard]] HiCacheCacheNode * mutable_node(HiCacheNodeId node_id);

    [[nodiscard]] bool contains_page(const std::string & page) const;
    [[nodiscard]] std::optional<HiCacheNodeId> node_for_page(const std::string & page) const;
    [[nodiscard]] std::vector<std::string> node_pages(HiCacheNodeId node_id) const;
    [[nodiscard]] std::vector<HiCacheNodeId> ancestor_node_ids(HiCacheNodeId terminal_node) const;
    [[nodiscard]] std::vector<std::string> flattened_pages(HiCacheNodeId terminal_node) const;

    void observe_page_path(const HiCachePagePath & path);

    [[nodiscard]] HiCachePathLookup lookup(const std::vector<std::string> & pages);
    [[nodiscard]] HiCachePathLookup lookup_peek(const std::vector<std::string> & pages);
    [[nodiscard]] std::vector<std::string> contiguous_prefix(const std::vector<std::string> & pages, bool include_device, bool include_host,
                                                             bool include_storage);

    HiCacheInsertResult insert_device_path(const std::vector<std::string> & pages, int64_t priority, bool dirty);
    HiCacheInsertResult insert_host_path(const std::vector<std::string> & pages, bool storage_readable);
    HiCacheInsertResult insert_host_path(const std::vector<std::string> & pages, const std::set<std::string> & visible_pages, bool storage_readable);

    void touch_chain(const std::vector<HiCacheNodeId> & chain);
    void add_lock_ref(const std::vector<HiCacheNodeId> & chain, const std::string & owner);
    void add_host_ref(const std::vector<HiCacheNodeId> & chain, const std::string & owner);
    void release_refs_by_owner(const std::string & owner);

    void mark_host_visible(HiCacheNodeId node_id, bool storage_readable);
    void clear_dirty(HiCacheNodeId node_id);
    void demote_device_to_host(HiCacheNodeId node_id, bool ensure_host);
    void remove_device_regular(HiCacheNodeId node_id);
    [[nodiscard]] HiCacheHostEvictionResult evict_host_leaf(HiCacheNodeId node_id);

private:
    std::vector<HiCacheCacheNode> nodes_;
    std::vector<HiCacheNodeSplitRecord> split_history_;
    std::unordered_map<std::string, HiCacheNodeId> page_to_node_;
    std::unordered_map<std::string, HiCacheProjectedPage> page_projection_;
    uint64_t access_clock_ = 0;

    [[nodiscard]] HiCacheNodeId create_child(HiCacheNodeId parent, std::vector<std::string> pages);
    [[nodiscard]] HiCacheNodeId insert_suffix(HiCacheNodeId parent, const std::vector<std::string> & suffix);
    [[nodiscard]] HiCacheNodeId split_child(HiCacheNodeId parent, HiCacheNodeId child, size_t split_pages);
    [[nodiscard]] HiCacheNodeSplitProjection split_projection(const std::vector<std::string> & pages, uint64_t depth_page_begin) const;
    [[nodiscard]] HiCachePathLookup lookup_impl(const std::vector<std::string> & pages, bool refresh_access);
    [[nodiscard]] bool has_backup_child(HiCacheNodeId node_id) const;
    void deactivate_subtree(HiCacheNodeId node_id, std::vector<HiCacheNodeId> & affected_nodes);
    void rebuild_page_index();
    void touch_node(HiCacheNodeId node_id);
};

} // namespace TraceGraph
