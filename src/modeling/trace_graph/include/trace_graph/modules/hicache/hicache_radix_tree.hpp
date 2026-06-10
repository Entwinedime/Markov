#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace TraceGraph {

class HiCacheRadixTree {
  public:
    HiCacheRadixTree();

    size_t longest_prefix_pages(const std::vector<std::string> & path) const;
    void insert_path(const std::vector<std::string> & path);
    void remove_pages(const std::vector<std::string> & pages);

    bool contains_page(const std::string & page) const;
    std::vector<std::string> leaf_group_for_page(const std::string & page) const;

  private:
    struct Node {
        size_t parent = 0;
        std::vector<std::string> key;
        std::map<std::string, size_t> children;
        bool active = true;
    };

    std::vector<Node> nodes_;
    std::set<std::string> known_pages_;
    std::map<std::string, std::vector<std::string>> leaf_group_by_page_;

    size_t create_child(size_t parent, const std::vector<std::string> & key);
    void insert_suffix(size_t node_id, const std::vector<std::string> & suffix);
    void rebuild_indexes();
};

} // namespace TraceGraph
