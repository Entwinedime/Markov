#include "trace_graph/modules/hicache/hicache_radix_tree.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace TraceGraph {

namespace {

size_t common_prefix_pages(const std::vector<std::string> & left, const std::vector<std::string> & right) {
    size_t count = 0;
    while (count < left.size() && count < right.size() && left[count] == right[count]) ++count;
    return count;
}

std::vector<std::string> slice_pages(const std::vector<std::string> & pages, size_t begin, size_t end) {
    if (begin >= pages.size() || begin >= end) return {};
    end = std::min(end, pages.size());
    return {pages.begin() + static_cast<long>(begin), pages.begin() + static_cast<long>(end)};
}

std::vector<std::string> suffix_pages(const std::vector<std::string> & pages, size_t begin) { return slice_pages(pages, begin, pages.size()); }

} // namespace

HiCacheRadixTree::HiCacheRadixTree() { nodes_.push_back(Node{}); }

bool HiCacheRadixTree::contains_page(const std::string & page) const { return !page.empty() && known_pages_.count(page) > 0; }

std::vector<std::string> HiCacheRadixTree::leaf_group_for_page(const std::string & page) const {
    auto it = leaf_group_by_page_.find(page);
    if (it == leaf_group_by_page_.end()) return {};
    return it->second;
}

size_t HiCacheRadixTree::longest_prefix_pages(const std::vector<std::string> & path) const {
    if (path.empty() || nodes_.empty()) return 0;
    size_t matched = 0;
    size_t node_id = 0;
    while (matched < path.size()) {
        if (node_id >= nodes_.size() || !nodes_[node_id].active) break;
        const auto child_it = nodes_[node_id].children.find(path[matched]);
        if (child_it == nodes_[node_id].children.end()) break;
        const auto child_id = child_it->second;
        if (child_id >= nodes_.size() || !nodes_[child_id].active) break;
        const auto & child_key = nodes_[child_id].key;
        const auto remaining = suffix_pages(path, matched);
        const auto shared = common_prefix_pages(child_key, remaining);
        matched += shared;
        if (shared < child_key.size()) break;
        node_id = child_id;
    }
    return matched;
}

size_t HiCacheRadixTree::create_child(size_t parent, const std::vector<std::string> & key) {
    if (key.empty()) return parent;
    Node node;
    node.parent = parent;
    node.key = key;
    const auto node_id = nodes_.size();
    nodes_.push_back(std::move(node));
    if (parent < nodes_.size() && nodes_[parent].active) nodes_[parent].children[key.front()] = node_id;
    return node_id;
}

void HiCacheRadixTree::insert_suffix(size_t node_id, const std::vector<std::string> & suffix) {
    if (suffix.empty() || node_id >= nodes_.size() || !nodes_[node_id].active) return;
    auto child_it = nodes_[node_id].children.find(suffix.front());
    if (child_it == nodes_[node_id].children.end()) {
        create_child(node_id, suffix);
        return;
    }

    const auto child_id = child_it->second;
    if (child_id >= nodes_.size() || !nodes_[child_id].active) {
        create_child(node_id, suffix);
        return;
    }

    const auto child_key = nodes_[child_id].key;
    const auto shared = common_prefix_pages(child_key, suffix);
    if (shared == child_key.size()) {
        insert_suffix(child_id, suffix_pages(suffix, shared));
        return;
    }

    const auto prefix = slice_pages(child_key, 0, shared);
    const auto old_suffix = suffix_pages(child_key, shared);
    const auto new_suffix = suffix_pages(suffix, shared);
    if (prefix.empty()) {
        create_child(node_id, suffix);
        return;
    }

    Node split;
    split.parent = node_id;
    split.key = prefix;
    const auto split_id = nodes_.size();
    nodes_.push_back(std::move(split));
    nodes_[node_id].children[prefix.front()] = split_id;

    nodes_[child_id].parent = split_id;
    nodes_[child_id].key = old_suffix;
    nodes_[split_id].children[old_suffix.front()] = child_id;
    if (!new_suffix.empty()) create_child(split_id, new_suffix);
}

void HiCacheRadixTree::insert_path(const std::vector<std::string> & path) {
    if (path.empty()) return;
    insert_suffix(0, path);
    rebuild_indexes();
}

void HiCacheRadixTree::remove_pages(const std::vector<std::string> & pages) {
    if (pages.empty()) return;
    const std::unordered_set<std::string> removed(pages.begin(), pages.end());
    for (size_t node_id = 1; node_id < nodes_.size(); ++node_id) {
        auto & node = nodes_[node_id];
        if (!node.active) continue;
        node.key.erase(std::remove_if(node.key.begin(), node.key.end(), [&](const std::string & page) { return removed.count(page) > 0; }), node.key.end());
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t node_id = 1; node_id < nodes_.size(); ++node_id) {
            auto & node = nodes_[node_id];
            if (!node.active || !node.key.empty()) continue;
            const auto parent = node.parent;
            for (const auto & [_, child_id] : node.children) {
                if (child_id < nodes_.size() && nodes_[child_id].active) nodes_[child_id].parent = parent;
            }
            node.active = false;
            changed = true;
        }
    }
    rebuild_indexes();
}

void HiCacheRadixTree::rebuild_indexes() {
    known_pages_.clear();
    leaf_group_by_page_.clear();
    for (auto & node : nodes_) node.children.clear();
    for (size_t node_id = 1; node_id < nodes_.size(); ++node_id) {
        auto & node = nodes_[node_id];
        if (!node.active || node.key.empty() || node.parent >= nodes_.size() || !nodes_[node.parent].active) continue;
        nodes_[node.parent].children[node.key.front()] = node_id;
        for (const auto & page : node.key) {
            if (page.empty()) continue;
            known_pages_.insert(page);
            leaf_group_by_page_[page] = node.key;
        }
    }
}

} // namespace TraceGraph
