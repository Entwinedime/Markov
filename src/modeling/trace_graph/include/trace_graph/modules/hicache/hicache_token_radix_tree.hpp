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
    HiCacheTokenRadixTree();

    size_t longest_prefix_tokens(const HiCacheTokenPath & path) const;
    size_t longest_prefix_pages(const HiCacheTokenPath & path, uint64_t page_size) const;
    void insert_path(const HiCacheTokenPath & path, const std::vector<std::string> & projected_pages);

    bool contains_page(const std::string & page) const;
    std::vector<std::string> leaf_group_for_page(const std::string & page) const;

  private:
    struct Node {
        size_t parent = 0;
        std::vector<uint32_t> key;
        std::map<uint32_t, size_t> children;
        bool active = true;
    };

    std::vector<Node> nodes_;
    std::set<std::string> known_pages_;
    std::map<std::string, std::vector<std::string>> leaf_group_by_page_;

    size_t create_child(size_t parent, const std::vector<uint32_t> & key);
    void insert_suffix(size_t node_id, const std::vector<uint32_t> & suffix);
    void register_leaf_pages(const std::vector<std::string> & projected_pages);
};

} // namespace TraceGraph
