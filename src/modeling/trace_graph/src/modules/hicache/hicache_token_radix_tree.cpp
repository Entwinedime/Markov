#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"

#include <algorithm>
#include <utility>

namespace TraceGraph {

namespace {

std::vector<uint32_t> flatten_tokens(const HiCacheTokenPath & path) {
    std::vector<uint32_t> tokens;
    for (const auto & token : path) {
        if (token.words.empty()) continue;
        tokens.push_back(token.words.front());
    }
    return tokens;
}

size_t common_prefix_tokens(const std::vector<uint32_t> & left, const std::vector<uint32_t> & right) {
    size_t count = 0;
    while (count < left.size() && count < right.size() && left[count] == right[count]) ++count;
    return count;
}

std::vector<uint32_t> slice_tokens(const std::vector<uint32_t> & tokens, size_t begin, size_t end) {
    if (begin >= tokens.size() || begin >= end) return {};
    end = std::min(end, tokens.size());
    return {tokens.begin() + static_cast<long>(begin), tokens.begin() + static_cast<long>(end)};
}

std::vector<uint32_t> suffix_tokens(const std::vector<uint32_t> & tokens, size_t begin) { return slice_tokens(tokens, begin, tokens.size()); }

size_t common_prefix_pages(const std::vector<std::string> & left, const std::vector<std::string> & right) {
    size_t count = 0;
    while (count < left.size() && count < right.size() && left[count] == right[count]) ++count;
    return count;
}

std::vector<std::string> slice_page_strings(const std::vector<std::string> & pages, size_t begin, size_t end) {
    if (begin >= pages.size() || begin >= end) return {};
    end = std::min(end, pages.size());
    return {pages.begin() + static_cast<long>(begin), pages.begin() + static_cast<long>(end)};
}

std::vector<std::string> suffix_page_strings(const std::vector<std::string> & pages, size_t begin) {
    return slice_page_strings(pages, begin, pages.size());
}

} // namespace

HiCacheTokenRadixTree::HiCacheTokenRadixTree() {
    nodes_.push_back(Node{});
    page_nodes_.push_back(PageNode{});
}

bool HiCacheTokenRadixTree::contains_page(const std::string & page) const { return !page.empty() && known_pages_.count(page) > 0; }

std::vector<std::string> HiCacheTokenRadixTree::leaf_group_for_page(const std::string & page) const {
    auto it = leaf_group_by_page_.find(page);
    if (it == leaf_group_by_page_.end()) return {};
    return it->second;
}

std::vector<std::vector<std::string>> HiCacheTokenRadixTree::host_eviction_leaf_groups(const std::set<std::string> & host_pages,
                                                                                       const std::set<std::string> & evicted_pages,
                                                                                       const std::set<std::string> & locked_pages) const {
    std::vector<std::vector<std::string>> groups;
    if (host_pages.empty() || page_nodes_.empty()) return groups;
    collect_host_eviction_leaf_groups(0, host_pages, evicted_pages, locked_pages, groups);
    return groups;
}

size_t HiCacheTokenRadixTree::longest_prefix_tokens(const HiCacheTokenPath & path) const {
    const auto tokens = flatten_tokens(path);
    if (tokens.empty() || nodes_.empty()) return 0;
    size_t matched = 0;
    size_t node_id = 0;
    while (matched < tokens.size()) {
        if (node_id >= nodes_.size() || !nodes_[node_id].active) break;
        const auto child_it = nodes_[node_id].children.find(tokens[matched]);
        if (child_it == nodes_[node_id].children.end()) break;
        const auto child_id = child_it->second;
        if (child_id >= nodes_.size() || !nodes_[child_id].active) break;
        const auto & child_key = nodes_[child_id].key;
        const auto remaining = suffix_tokens(tokens, matched);
        const auto shared = common_prefix_tokens(child_key, remaining);
        matched += shared;
        if (shared < child_key.size()) break;
        node_id = child_id;
    }
    return matched;
}

size_t HiCacheTokenRadixTree::longest_prefix_pages(const HiCacheTokenPath & path, uint64_t page_size) const {
    if (page_size == 0) return 0;
    return longest_prefix_tokens(path) / static_cast<size_t>(page_size);
}

size_t HiCacheTokenRadixTree::create_child(size_t parent, const std::vector<uint32_t> & key) {
    if (key.empty()) return parent;
    Node node;
    node.parent = parent;
    node.key = key;
    const auto node_id = nodes_.size();
    nodes_.push_back(std::move(node));
    if (parent < nodes_.size() && nodes_[parent].active) nodes_[parent].children[key.front()] = node_id;
    return node_id;
}

void HiCacheTokenRadixTree::insert_suffix(size_t node_id, const std::vector<uint32_t> & suffix) {
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
    const auto shared = common_prefix_tokens(child_key, suffix);
    if (shared == child_key.size()) {
        insert_suffix(child_id, suffix_tokens(suffix, shared));
        return;
    }

    const auto prefix = slice_tokens(child_key, 0, shared);
    const auto old_suffix = suffix_tokens(child_key, shared);
    const auto new_suffix = suffix_tokens(suffix, shared);
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

size_t HiCacheTokenRadixTree::create_page_child(size_t parent, const std::vector<std::string> & pages) {
    if (pages.empty()) return parent;
    PageNode node;
    node.parent = parent;
    node.pages = pages;
    const auto node_id = page_nodes_.size();
    page_nodes_.push_back(std::move(node));
    if (parent < page_nodes_.size() && page_nodes_[parent].active) page_nodes_[parent].children[pages.front()] = node_id;
    return node_id;
}

void HiCacheTokenRadixTree::insert_page_suffix(size_t node_id, const std::vector<std::string> & suffix) {
    if (suffix.empty() || node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return;
    auto child_it = page_nodes_[node_id].children.find(suffix.front());
    if (child_it == page_nodes_[node_id].children.end()) {
        create_page_child(node_id, suffix);
        return;
    }

    const auto child_id = child_it->second;
    if (child_id >= page_nodes_.size() || !page_nodes_[child_id].active) {
        create_page_child(node_id, suffix);
        return;
    }

    const auto child_pages = page_nodes_[child_id].pages;
    const auto shared = common_prefix_pages(child_pages, suffix);
    if (shared == child_pages.size()) {
        insert_page_suffix(child_id, suffix_page_strings(suffix, shared));
        return;
    }

    const auto prefix = slice_page_strings(child_pages, 0, shared);
    const auto old_suffix = suffix_page_strings(child_pages, shared);
    const auto new_suffix = suffix_page_strings(suffix, shared);
    if (prefix.empty()) {
        create_page_child(node_id, suffix);
        return;
    }

    PageNode split;
    split.parent = node_id;
    split.pages = prefix;
    const auto split_id = page_nodes_.size();
    page_nodes_.push_back(std::move(split));
    page_nodes_[node_id].children[prefix.front()] = split_id;

    page_nodes_[child_id].parent = split_id;
    page_nodes_[child_id].pages = old_suffix;
    page_nodes_[split_id].children[old_suffix.front()] = child_id;
    if (!new_suffix.empty()) create_page_child(split_id, new_suffix);
}

void HiCacheTokenRadixTree::register_leaf_pages(const std::vector<std::string> & projected_pages) {
    if (projected_pages.empty()) return;
    insert_page_suffix(0, projected_pages);
    for (const auto & page : projected_pages) {
        if (page.empty()) continue;
        known_pages_.insert(page);
        leaf_group_by_page_[page] = projected_pages;
    }
}

bool HiCacheTokenRadixTree::page_node_has_host_value(size_t node_id, const std::set<std::string> & host_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    const auto & node = page_nodes_[node_id];
    if (node.pages.empty()) return false;
    for (const auto & page : node.pages) {
        if (host_pages.count(page) == 0) return false;
    }
    return true;
}

bool HiCacheTokenRadixTree::page_subtree_has_host_value(size_t node_id, const std::set<std::string> & host_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    if (page_node_has_host_value(node_id, host_pages)) return true;
    const auto & node = page_nodes_[node_id];
    for (const auto & [page, child_id] : node.children) {
        (void)page;
        if (page_subtree_has_host_value(child_id, host_pages)) return true;
    }
    return false;
}

bool HiCacheTokenRadixTree::page_node_evicted(size_t node_id, const std::set<std::string> & evicted_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    const auto & node = page_nodes_[node_id];
    if (node.pages.empty()) return false;
    for (const auto & page : node.pages) {
        if (evicted_pages.count(page) == 0) return false;
    }
    return true;
}

bool HiCacheTokenRadixTree::page_node_locked(size_t node_id, const std::set<std::string> & locked_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    for (const auto & page : page_nodes_[node_id].pages) {
        if (locked_pages.count(page) > 0) return true;
    }
    return false;
}

void HiCacheTokenRadixTree::collect_host_eviction_leaf_groups(size_t node_id, const std::set<std::string> & host_pages,
                                                              const std::set<std::string> & evicted_pages,
                                                              const std::set<std::string> & locked_pages,
                                                              std::vector<std::vector<std::string>> & groups) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return;
    const auto & node = page_nodes_[node_id];
    const bool node_eligible = node_id != 0 && page_node_has_host_value(node_id, host_pages) && page_node_evicted(node_id, evicted_pages) &&
                               !page_node_locked(node_id, locked_pages);

    bool has_host_child = false;
    for (const auto & [page, child_id] : node.children) {
        (void)page;
        if (page_subtree_has_host_value(child_id, host_pages)) {
            has_host_child = true;
            break;
        }
    }
    if (node_eligible && !has_host_child) {
        groups.push_back(node.pages);
        return;
    }

    for (const auto & [page, child_id] : node.children) {
        (void)page;
        collect_host_eviction_leaf_groups(child_id, host_pages, evicted_pages, locked_pages, groups);
    }
}

void HiCacheTokenRadixTree::insert_path(const HiCacheTokenPath & path, const std::vector<std::string> & projected_pages) {
    const auto tokens = flatten_tokens(path);
    if (tokens.empty()) return;
    insert_suffix(0, tokens);
    register_leaf_pages(projected_pages);
}

} // namespace TraceGraph
