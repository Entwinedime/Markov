#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace TraceGraph {

class HiCacheTokenRadixTree {
  public:
    struct PagePathMatch {
        size_t terminal_node = 0;
        std::vector<std::string> matched_pages;
        std::vector<std::vector<std::string>> ancestor_page_groups;
    };

    HiCacheTokenRadixTree();

    size_t longest_prefix_tokens(const HiCacheTokenPath & path) const;
    size_t longest_prefix_pages(const HiCacheTokenPath & path, uint64_t page_size) const;
    PagePathMatch match_page_path(const std::vector<std::string> & projected_pages) const;
    PagePathMatch insert_path(const HiCacheTokenPath & path, const std::vector<std::string> & projected_pages);
    PagePathMatch insert_page_path(const std::vector<std::string> & projected_pages);

    bool contains_page(const std::string & page) const;
    std::vector<std::string> node_pages(size_t node_id) const;
    std::vector<size_t> ancestor_node_ids(size_t terminal_node) const;
    std::vector<std::vector<std::string>> ancestor_page_groups(size_t terminal_node) const;
    std::vector<std::string> flattened_ancestor_pages(size_t terminal_node) const;
    std::vector<std::string> page_path_for_page(const std::string & page) const;
    std::vector<std::string> leaf_group_for_page(const std::string & page) const;
    std::vector<std::vector<std::string>> device_eviction_leaf_groups(const std::set<std::string> & device_pages,
                                                                      const std::set<std::string> & locked_pages) const;
    std::vector<std::vector<std::string>> host_eviction_leaf_groups(const std::set<std::string> & host_pages, const std::set<std::string> & evicted_pages,
                                                                    const std::set<std::string> & locked_pages) const;

  private:
    struct Node {
        size_t parent = 0;
        std::vector<uint32_t> key;
        std::map<uint32_t, size_t> children;
        bool active = true;
    };

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
