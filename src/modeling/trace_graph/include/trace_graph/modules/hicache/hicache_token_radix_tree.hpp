#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief 同时维护 token-level 和 page-level 的 HiCache radix topology。
 *
 * token tree 用于计算最长 token/page 前缀；page tree 用于恢复 ancestor page group、
 * host/device leaf group，以及 capacity eviction 的拓扑边界。状态模型依赖这些边界
 * 保持 SGLang radix cache 的块状 eviction 语义。
 */
class HiCacheTokenRadixTree {
public:
    /**
     * @brief page path match 的拓扑结果。
     *
     * matched_pages 是连续匹配前缀；ancestor_page_groups 保留从 root 到 terminal
     * node 的 page 分组，供 lock/ref 和 eviction 维护节点粒度不变量。
     */
    struct PagePathMatch {
        size_t terminal_node = 0;
        std::vector<std::string> matched_pages;
        std::vector<std::vector<std::string>> ancestor_page_groups;
    };

    HiCacheTokenRadixTree();

    /** @brief 返回 token tree 中匹配的最长 token 前缀长度。 */
    [[nodiscard]] size_t longest_prefix_tokens(const HiCacheTokenPath & path) const;

    /** @brief 将最长 token 前缀按 page_size 折算成 page 前缀长度。 */
    [[nodiscard]] size_t longest_prefix_pages(const HiCacheTokenPath & path, uint64_t page_size) const;

    /** @brief 在 page tree 上匹配 projected page path。 */
    [[nodiscard]] PagePathMatch match_page_path(const std::vector<std::string> & projected_pages) const;

    /** @brief 同时插入 token path 和对应 page path。 */
    [[nodiscard]] PagePathMatch insert_path(const HiCacheTokenPath & path, const std::vector<std::string> & projected_pages);

    /** @brief 只插入 page topology，用于 host-visible/storage-known 路径恢复。 */
    [[nodiscard]] PagePathMatch insert_page_path(const std::vector<std::string> & projected_pages);

    /** @brief 判断 page 是否存在于当前 modeled topology。 */
    [[nodiscard]] bool contains_page(const std::string & page) const;

    /** @brief 返回 page tree 指定节点持有的 page group。 */
    [[nodiscard]] std::vector<std::string> node_pages(size_t node_id) const;

    /** @brief 返回 terminal node 到 root 的有效 ancestor node id。 */
    [[nodiscard]] std::vector<size_t> ancestor_node_ids(size_t terminal_node) const;

    /** @brief 返回 terminal node 的 ancestor page group 链。 */
    [[nodiscard]] std::vector<std::vector<std::string>> ancestor_page_groups(size_t terminal_node) const;

    /** @brief 将 ancestor page group 链展平成 page path。 */
    [[nodiscard]] std::vector<std::string> flattened_ancestor_pages(size_t terminal_node) const;

    /** @brief 返回从 root 到 page 所在节点的完整 page path。 */
    [[nodiscard]] std::vector<std::string> page_path_for_page(const std::string & page) const;

    /** @brief 返回 page 所在 leaf group，用于成组 eviction。 */
    [[nodiscard]] std::vector<std::string> leaf_group_for_page(const std::string & page) const;

    /** @brief 返回 device eviction 可选择的 leaf group，排除 locked page。 */
    [[nodiscard]] std::vector<std::vector<std::string>> device_eviction_leaf_groups(const std::set<std::string> & device_pages,
                                                                                    const std::set<std::string> & locked_pages) const;

    /** @brief 返回 host eviction 可选择的 leaf group，要求 page 已经处于 evicted 集合且未被保护。 */
    [[nodiscard]] std::vector<std::vector<std::string>> host_eviction_leaf_groups(const std::set<std::string> & host_pages,
                                                                                  const std::set<std::string> & evicted_pages,
                                                                                  const std::set<std::string> & locked_pages) const;

private:
    /** @brief token radix 节点；key 是压缩边上的 token 序列。 */
    struct Node {
        size_t parent = 0;
        std::vector<uint32_t> key;
        std::map<uint32_t, size_t> children;
        bool active = true;
    };

    /** @brief page radix 节点；pages 是压缩边上的 page group。 */
    struct PageNode {
        size_t parent = 0;
        std::vector<std::string> pages;
        std::map<std::string, size_t> children;
        bool active = true;
    };

    std::vector<Node> nodes_;
    std::vector<PageNode> page_nodes_;
    std::set<std::string> known_pages_;
    std::map<std::string, std::vector<std::string>> leaf_group_by_page_;
    std::map<std::string, size_t> page_node_by_page_;

    size_t create_child(size_t parent, const std::vector<uint32_t> & key);
    void insert_suffix(size_t node_id, const std::vector<uint32_t> & suffix);
    size_t create_page_child(size_t parent, const std::vector<std::string> & pages);
    size_t insert_page_suffix(size_t node_id, const std::vector<std::string> & suffix);
    void rebuild_page_group_index();
    bool page_node_has_device_value(size_t node_id, const std::set<std::string> & device_pages) const;
    bool page_subtree_has_device_value(size_t node_id, const std::set<std::string> & device_pages) const;
    bool page_node_has_host_value(size_t node_id, const std::set<std::string> & host_pages) const;
    bool page_subtree_has_host_value(size_t node_id, const std::set<std::string> & host_pages) const;
    bool page_node_evicted(size_t node_id, const std::set<std::string> & evicted_pages) const;
    bool page_node_locked(size_t node_id, const std::set<std::string> & locked_pages) const;
    void collect_device_eviction_leaf_groups(size_t node_id, const std::set<std::string> & device_pages, const std::set<std::string> & locked_pages,
                                             std::vector<std::vector<std::string>> & groups) const;
    void collect_host_eviction_leaf_groups(size_t node_id, const std::set<std::string> & host_pages, const std::set<std::string> & evicted_pages,
                                           const std::set<std::string> & locked_pages, std::vector<std::vector<std::string>> & groups) const;
};

} // namespace TraceGraph
