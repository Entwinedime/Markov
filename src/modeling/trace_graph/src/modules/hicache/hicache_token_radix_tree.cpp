/**
 * @file
 * @brief HiCache token/page radix topology 与 leaf-group eviction helper。
 */
#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"

#include <algorithm>
#include <utility>

namespace TraceGraph {

namespace {

/**
 * @brief 将 HiCacheTokenPath 压平成单 word token 序列。
 *
 * 当前 HiCache page hash 和 radix 匹配只使用 token 的首个 word；空 token 被跳过，
 * 保证 parser 接纳到的异常 token 不会破坏 radix 结构。
 */
std::vector<uint32_t> flatten_tokens(const HiCacheTokenPath & path) {
    std::vector<uint32_t> tokens;
    for (const auto & token : path) {
        if (token.words.empty()) continue;
        tokens.push_back(token.words.front());
    }
    return tokens;
}

/** @brief 返回两段 token 序列的公共前缀长度。 */
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

/** @brief 返回两段 page path 的公共前缀长度。 */
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

std::vector<std::string> suffix_page_strings(const std::vector<std::string> & pages, size_t begin) { return slice_page_strings(pages, begin, pages.size()); }

} // namespace

/** @brief 初始化 token radix 和 page radix 的 root 节点。 */
HiCacheTokenRadixTree::HiCacheTokenRadixTree() {
    nodes_.push_back(Node{});
    page_nodes_.push_back(PageNode{});
}

/** @brief 判断 page 是否已经出现在 modeled page topology 中。 */
bool HiCacheTokenRadixTree::contains_page(const std::string & page) const { return !page.empty() && known_pages_.count(page) > 0; }

/** @brief 返回 page 所属 leaf group；找不到时返回空集合。 */
std::vector<std::string> HiCacheTokenRadixTree::leaf_group_for_page(const std::string & page) const {
    auto it = leaf_group_by_page_.find(page);
    if (it == leaf_group_by_page_.end()) return {};
    return it->second;
}

/** @brief 返回从 root 到 page 所在节点的完整 page path。 */
std::vector<std::string> HiCacheTokenRadixTree::page_path_for_page(const std::string & page) const {
    auto it = page_node_by_page_.find(page);
    if (it == page_node_by_page_.end()) return {};
    return flattened_ancestor_pages(it->second);
}

/** @brief 返回 page radix 节点持有的压缩 page group。 */
std::vector<std::string> HiCacheTokenRadixTree::node_pages(size_t node_id) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return {};
    return page_nodes_[node_id].pages;
}

/** @brief 返回 terminal node 的 ancestor chain，顺序为 root 到 leaf。 */
std::vector<size_t> HiCacheTokenRadixTree::ancestor_node_ids(size_t terminal_node) const {
    std::vector<size_t> ids;
    if (terminal_node == 0 || terminal_node >= page_nodes_.size() || !page_nodes_[terminal_node].active) return ids;
    size_t node_id = terminal_node;
    while (node_id != 0 && node_id < page_nodes_.size() && page_nodes_[node_id].active) {
        ids.push_back(node_id);
        node_id = page_nodes_[node_id].parent;
    }
    std::reverse(ids.begin(), ids.end());
    return ids;
}

/** @brief 将 ancestor node chain 转换成 page group chain。 */
std::vector<std::vector<std::string>> HiCacheTokenRadixTree::ancestor_page_groups(size_t terminal_node) const {
    std::vector<std::vector<std::string>> groups;
    for (const auto node_id : ancestor_node_ids(terminal_node)) {
        auto pages = node_pages(node_id);
        if (!pages.empty()) groups.push_back(std::move(pages));
    }
    return groups;
}

/** @brief 展平 ancestor page group，供 request lock/ref 使用。 */
std::vector<std::string> HiCacheTokenRadixTree::flattened_ancestor_pages(size_t terminal_node) const {
    std::vector<std::string> pages;
    for (const auto & group : ancestor_page_groups(terminal_node)) pages.insert(pages.end(), group.begin(), group.end());
    return pages;
}

/**
 * @brief 收集 host cleanup 可以回收的 leaf group。
 *
 * host victim 必须同时满足：当前在 host_pages 中、已经被 L1 eviction 标记、且没有被
 * request ref/lock 保护。
 */
std::vector<std::vector<std::string>> HiCacheTokenRadixTree::host_eviction_leaf_groups(const std::set<std::string> & host_pages,
                                                                                       const std::set<std::string> & evicted_pages,
                                                                                       const std::set<std::string> & locked_pages) const {
    std::vector<std::vector<std::string>> groups;
    if (host_pages.empty() || page_nodes_.empty()) return groups;
    collect_host_eviction_leaf_groups(0, host_pages, evicted_pages, locked_pages, groups);
    return groups;
}

/**
 * @brief 收集 device L1 cleanup 可以回收的 leaf group。
 *
 * device victim 不要求已经 evicted，但不能包含 locked page；leaf group 粒度用于保留
 * radix cache 的块状回收语义。
 */
std::vector<std::vector<std::string>> HiCacheTokenRadixTree::device_eviction_leaf_groups(const std::set<std::string> & device_pages,
                                                                                         const std::set<std::string> & locked_pages) const {
    std::vector<std::vector<std::string>> groups;
    if (device_pages.empty() || page_nodes_.empty()) return groups;
    collect_device_eviction_leaf_groups(0, device_pages, locked_pages, groups);
    return groups;
}

/** @brief 在 token radix 中查找最长匹配前缀。 */
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

/** @brief 把 token 前缀按 page_size 折算为 page 前缀。 */
size_t HiCacheTokenRadixTree::longest_prefix_pages(const HiCacheTokenPath & path, uint64_t page_size) const {
    if (page_size == 0) return 0;
    return longest_prefix_tokens(path) / static_cast<size_t>(page_size);
}

/** @brief 创建一条压缩 token 边。 */
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

/**
 * @brief 向 token radix 插入 suffix，必要时拆分压缩边。
 *
 * split 后旧 child 和新 suffix 共享公共 prefix；这是 longest-prefix lookup 的核心
 * 不变量。
 */
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

/** @brief 创建一条压缩 page 边。 */
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

/**
 * @brief 向 page radix 插入 suffix，必要时拆分 page group。
 *
 * page tree 的 split 逻辑与 token tree 对齐，但 key 是 scoped page id。返回值是插入
 * 或匹配到的 terminal node。
 */
size_t HiCacheTokenRadixTree::insert_page_suffix(size_t node_id, const std::vector<std::string> & suffix) {
    if (suffix.empty() || node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return node_id;
    auto child_it = page_nodes_[node_id].children.find(suffix.front());
    if (child_it == page_nodes_[node_id].children.end()) { return create_page_child(node_id, suffix); }

    const auto child_id = child_it->second;
    if (child_id >= page_nodes_.size() || !page_nodes_[child_id].active) { return create_page_child(node_id, suffix); }

    const auto child_pages = page_nodes_[child_id].pages;
    const auto shared = common_prefix_pages(child_pages, suffix);
    if (shared == child_pages.size()) {
        auto remaining = suffix_page_strings(suffix, shared);
        if (remaining.empty()) return child_id;
        return insert_page_suffix(child_id, remaining);
    }

    const auto prefix = slice_page_strings(child_pages, 0, shared);
    const auto old_suffix = suffix_page_strings(child_pages, shared);
    const auto new_suffix = suffix_page_strings(suffix, shared);
    if (prefix.empty()) { return create_page_child(node_id, suffix); }

    PageNode split;
    split.parent = node_id;
    split.pages = prefix;
    const auto split_id = page_nodes_.size();
    page_nodes_.push_back(std::move(split));
    page_nodes_[node_id].children[prefix.front()] = split_id;

    page_nodes_[child_id].parent = split_id;
    page_nodes_[child_id].pages = old_suffix;
    page_nodes_[split_id].children[old_suffix.front()] = child_id;
    if (!new_suffix.empty()) return create_page_child(split_id, new_suffix);
    return split_id;
}

/**
 * @brief 重建 page 到 leaf group / node 的反向索引。
 *
 * page radix 插入可能拆分节点，因此每次 topology 变化后重建索引，保证 eviction 和
 * page_path 查询看到一致结构。
 */
void HiCacheTokenRadixTree::rebuild_page_group_index() {
    leaf_group_by_page_.clear();
    page_node_by_page_.clear();
    known_pages_.clear();
    for (size_t node_id = 0; node_id < page_nodes_.size(); ++node_id) {
        const auto & node = page_nodes_[node_id];
        if (!node.active || node.pages.empty()) continue;
        for (const auto & page : node.pages) {
            if (page.empty()) continue;
            known_pages_.insert(page);
            page_node_by_page_[page] = node_id;
            if (node.children.empty()) leaf_group_by_page_[page] = node.pages;
        }
    }
}

/** @brief 在 page radix 中查找 projected page path 的最长连续前缀。 */
HiCacheTokenRadixTree::PagePathMatch HiCacheTokenRadixTree::match_page_path(const std::vector<std::string> & projected_pages) const {
    PagePathMatch result;
    if (projected_pages.empty() || page_nodes_.empty()) return result;
    size_t node_id = 0;
    size_t matched = 0;
    while (matched < projected_pages.size()) {
        if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) break;
        const auto child_it = page_nodes_[node_id].children.find(projected_pages[matched]);
        if (child_it == page_nodes_[node_id].children.end()) break;
        const auto child_id = child_it->second;
        if (child_id >= page_nodes_.size() || !page_nodes_[child_id].active) break;
        const auto remaining = suffix_page_strings(projected_pages, matched);
        const auto shared = common_prefix_pages(page_nodes_[child_id].pages, remaining);
        result.matched_pages.insert(
            result.matched_pages.end(), projected_pages.begin() + static_cast<long>(matched), projected_pages.begin() + static_cast<long>(matched + shared));
        matched += shared;
        if (shared < page_nodes_[child_id].pages.size()) break;
        node_id = child_id;
        result.terminal_node = node_id;
    }
    result.ancestor_page_groups = ancestor_page_groups(result.terminal_node);
    return result;
}

/** @brief 插入 page path 并返回 terminal node 的 ancestor groups。 */
HiCacheTokenRadixTree::PagePathMatch HiCacheTokenRadixTree::insert_page_path(const std::vector<std::string> & projected_pages) {
    PagePathMatch result;
    if (projected_pages.empty()) return result;
    const auto terminal = insert_page_suffix(0, projected_pages);
    rebuild_page_group_index();
    result.terminal_node = terminal;
    result.matched_pages = projected_pages;
    result.ancestor_page_groups = ancestor_page_groups(terminal);
    return result;
}

/** @brief 判断节点自身 page group 是否完整 resident 于 host set。 */
bool HiCacheTokenRadixTree::page_node_has_host_value(size_t node_id, const std::set<std::string> & host_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    const auto & node = page_nodes_[node_id];
    if (node.pages.empty()) return false;
    for (const auto & page : node.pages) {
        if (host_pages.count(page) == 0) return false;
    }
    return true;
}

/** @brief 判断节点自身 page group 是否完整 resident 于 device set。 */
bool HiCacheTokenRadixTree::page_node_has_device_value(size_t node_id, const std::set<std::string> & device_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    const auto & node = page_nodes_[node_id];
    if (node.pages.empty()) return false;
    for (const auto & page : node.pages) {
        if (device_pages.count(page) == 0) return false;
    }
    return true;
}

/** @brief 判断子树中是否存在 device-resident page group。 */
bool HiCacheTokenRadixTree::page_subtree_has_device_value(size_t node_id, const std::set<std::string> & device_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    if (page_node_has_device_value(node_id, device_pages)) return true;
    const auto & node = page_nodes_[node_id];
    for (const auto & [page, child_id] : node.children) {
        (void)page;
        if (page_subtree_has_device_value(child_id, device_pages)) return true;
    }
    return false;
}

/** @brief 判断子树中是否存在 host-resident page group。 */
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

/** @brief 判断节点自身 page group 是否全部带有 evicted 标记。 */
bool HiCacheTokenRadixTree::page_node_evicted(size_t node_id, const std::set<std::string> & evicted_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    const auto & node = page_nodes_[node_id];
    if (node.pages.empty()) return false;
    for (const auto & page : node.pages) {
        if (evicted_pages.count(page) == 0) return false;
    }
    return true;
}

/** @brief 判断节点自身 page group 是否包含受保护 page。 */
bool HiCacheTokenRadixTree::page_node_locked(size_t node_id, const std::set<std::string> & locked_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    for (const auto & page : page_nodes_[node_id].pages) {
        if (locked_pages.count(page) > 0) return true;
    }
    return false;
}

/**
 * @brief 递归收集 device eviction 的最深可回收 group。
 *
 * 如果当前节点可回收但子树中仍有 device value，则继续向下，避免过早回收包含活跃
 * 子路径的祖先 group。
 */
void HiCacheTokenRadixTree::collect_device_eviction_leaf_groups(size_t node_id, const std::set<std::string> & device_pages,
                                                                const std::set<std::string> & locked_pages,
                                                                std::vector<std::vector<std::string>> & groups) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return;
    const auto & node = page_nodes_[node_id];
    const bool node_eligible = node_id != 0 && page_node_has_device_value(node_id, device_pages) && !page_node_locked(node_id, locked_pages);

    bool has_device_child = false;
    for (const auto & [page, child_id] : node.children) {
        (void)page;
        if (page_subtree_has_device_value(child_id, device_pages)) {
            has_device_child = true;
            break;
        }
    }
    if (node_eligible && !has_device_child) {
        groups.push_back(node.pages);
        return;
    }

    for (const auto & [page, child_id] : node.children) {
        (void)page;
        collect_device_eviction_leaf_groups(child_id, device_pages, locked_pages, groups);
    }
}

/**
 * @brief 递归收集 host eviction 的最深可回收 group。
 *
 * host group 还必须处于 evicted_pages 中；这保证 host cleanup 只释放已经从 device
 * 侧失效、且仍有 storage-backed 语义的 page。
 */
void HiCacheTokenRadixTree::collect_host_eviction_leaf_groups(size_t node_id, const std::set<std::string> & host_pages,
                                                              const std::set<std::string> & evicted_pages, const std::set<std::string> & locked_pages,
                                                              std::vector<std::vector<std::string>> & groups) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return;
    const auto & node = page_nodes_[node_id];
    const bool node_eligible =
        node_id != 0 && page_node_has_host_value(node_id, host_pages) && page_node_evicted(node_id, evicted_pages) && !page_node_locked(node_id, locked_pages);

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

/**
 * @brief 插入 token path 及其 target page projection。
 *
 * token radix 服务 longest-prefix token/page 估算，page radix 服务 ancestor group 和
 * eviction topology；两者必须在同一次 request insert 中保持同步。
 */
HiCacheTokenRadixTree::PagePathMatch HiCacheTokenRadixTree::insert_path(const HiCacheTokenPath & path, const std::vector<std::string> & projected_pages) {
    const auto tokens = flatten_tokens(path);
    if (!tokens.empty()) insert_suffix(0, tokens);
    return insert_page_path(projected_pages);
}

} // namespace TraceGraph
