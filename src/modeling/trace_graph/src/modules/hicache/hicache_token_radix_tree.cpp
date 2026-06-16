/**
 * @file
 * @brief HiCache token/page radix topology 与 leaf-group eviction helper。
 */
#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
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
    tokens.reserve(path.size());
    auto first_words = path | std::views::filter([](const HiCacheToken & token) { return !token.words.empty(); })
                       | std::views::transform([](const HiCacheToken & token) { return token.words.front(); });
    std::ranges::copy(first_words, std::back_inserter(tokens));
    return tokens;
}

/** @brief 返回两段序列的公共前缀长度。 */
template <typename T> size_t common_prefix_size(const std::vector<T> & left, const std::vector<T> & right) {
    const auto [left_it, right_it] = std::ranges::mismatch(left, right);
    (void)right_it;
    return static_cast<size_t>(std::ranges::distance(left.begin(), left_it));
}

/** @brief 从 vector 中复制半开区间，避免 token/page 两套切片逻辑分叉。 */
template <typename T> std::vector<T> slice_vector(const std::vector<T> & values, size_t begin, size_t end) {
    if (begin >= values.size() || begin >= end) return {};
    end = std::min(end, values.size());

    std::vector<T> result;
    result.reserve(end - begin);
    auto slice = values | std::views::drop(static_cast<std::ranges::range_difference_t<std::vector<T>>>(begin))
                 | std::views::take(static_cast<std::ranges::range_difference_t<std::vector<T>>>(end - begin));
    std::ranges::copy(slice, std::back_inserter(result));
    return result;
}

/** @brief 返回从 begin 到末尾的 suffix。 */
template <typename T> std::vector<T> suffix_vector(const std::vector<T> & values, size_t begin) { return slice_vector(values, begin, values.size()); }

/** @brief 判断 page group 是否全部存在于指定集合。 */
bool all_pages_in(const std::vector<std::string> & pages, const std::set<std::string> & values) {
    return !pages.empty() && std::ranges::all_of(pages, [&](const auto & page) { return values.contains(page); });
}

/** @brief 判断 page group 是否包含指定集合中的任意 page。 */
bool any_page_in(const std::vector<std::string> & pages, const std::set<std::string> & values) {
    return std::ranges::any_of(pages, [&](const auto & page) { return values.contains(page); });
}

/** @brief 将 path 的一段连续 page 追加到输出。 */
void append_page_slice(const std::vector<std::string> & source, size_t begin, size_t count, std::vector<std::string> & output) {
    auto slice = source | std::views::drop(static_cast<std::ranges::range_difference_t<std::vector<std::string>>>(begin))
                 | std::views::take(static_cast<std::ranges::range_difference_t<std::vector<std::string>>>(count));
    std::ranges::copy(slice, std::back_inserter(output));
}

} // namespace

/** @brief 初始化 token radix 和 page radix 的 root 节点。 */
HiCacheTokenRadixTree::HiCacheTokenRadixTree() {
    nodes_.push_back(Node{});
    page_nodes_.push_back(PageNode{});
}

/** @brief 判断 page 是否已经出现在 modeled page topology 中。 */
bool HiCacheTokenRadixTree::contains_page(const std::string & page) const { return !page.empty() && known_pages_.contains(page); }

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
    std::ranges::for_each(ancestor_page_groups(terminal_node), [&](const auto & group) { pages.insert(pages.end(), group.begin(), group.end()); });
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
        const auto remaining = suffix_vector(tokens, matched);
        const auto shared = common_prefix_size(child_key, remaining);
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
    const auto shared = common_prefix_size(child_key, suffix);
    if (shared == child_key.size()) {
        insert_suffix(child_id, suffix_vector(suffix, shared));
        return;
    }

    const auto prefix = slice_vector(child_key, 0, shared);
    const auto old_suffix = suffix_vector(child_key, shared);
    const auto new_suffix = suffix_vector(suffix, shared);
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
    const auto shared = common_prefix_size(child_pages, suffix);
    if (shared == child_pages.size()) {
        auto remaining = suffix_vector(suffix, shared);
        if (remaining.empty()) return child_id;
        return insert_page_suffix(child_id, remaining);
    }

    const auto prefix = slice_vector(child_pages, 0, shared);
    const auto old_suffix = suffix_vector(child_pages, shared);
    const auto new_suffix = suffix_vector(suffix, shared);
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
    for (const auto node_id : std::views::iota(size_t{ 0 }, page_nodes_.size())) {
        const auto & node = page_nodes_[node_id];
        if (!node.active || node.pages.empty()) continue;
        auto valid_pages = node.pages | std::views::filter([](const auto & page) { return !page.empty(); });
        std::ranges::for_each(valid_pages, [&](const auto & page) {
            known_pages_.insert(page);
            page_node_by_page_[page] = node_id;
            if (node.children.empty()) leaf_group_by_page_[page] = node.pages;
        });
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
        const auto remaining = suffix_vector(projected_pages, matched);
        const auto shared = common_prefix_size(page_nodes_[child_id].pages, remaining);
        append_page_slice(projected_pages, matched, shared, result.matched_pages);
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
    return all_pages_in(page_nodes_[node_id].pages, host_pages);
}

/** @brief 判断节点自身 page group 是否完整 resident 于 device set。 */
bool HiCacheTokenRadixTree::page_node_has_device_value(size_t node_id, const std::set<std::string> & device_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    return all_pages_in(page_nodes_[node_id].pages, device_pages);
}

/** @brief 判断子树中是否存在 device-resident page group。 */
bool HiCacheTokenRadixTree::page_subtree_has_device_value(size_t node_id, const std::set<std::string> & device_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    if (page_node_has_device_value(node_id, device_pages)) return true;
    const auto & node = page_nodes_[node_id];
    return std::ranges::any_of(node.children | std::views::values, [&](const auto child_id) { return page_subtree_has_device_value(child_id, device_pages); });
}

/** @brief 判断子树中是否存在 host-resident page group。 */
bool HiCacheTokenRadixTree::page_subtree_has_host_value(size_t node_id, const std::set<std::string> & host_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    if (page_node_has_host_value(node_id, host_pages)) return true;
    const auto & node = page_nodes_[node_id];
    return std::ranges::any_of(node.children | std::views::values, [&](const auto child_id) { return page_subtree_has_host_value(child_id, host_pages); });
}

/** @brief 判断节点自身 page group 是否全部带有 evicted 标记。 */
bool HiCacheTokenRadixTree::page_node_evicted(size_t node_id, const std::set<std::string> & evicted_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    return all_pages_in(page_nodes_[node_id].pages, evicted_pages);
}

/** @brief 判断节点自身 page group 是否包含受保护 page。 */
bool HiCacheTokenRadixTree::page_node_locked(size_t node_id, const std::set<std::string> & locked_pages) const {
    if (node_id >= page_nodes_.size() || !page_nodes_[node_id].active) return false;
    return any_page_in(page_nodes_[node_id].pages, locked_pages);
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

    const bool has_device_child =
        std::ranges::any_of(node.children | std::views::values, [&](const auto child_id) { return page_subtree_has_device_value(child_id, device_pages); });
    if (node_eligible && !has_device_child) {
        groups.push_back(node.pages);
        return;
    }

    std::ranges::for_each(node.children | std::views::values,
                          [&](const auto child_id) { collect_device_eviction_leaf_groups(child_id, device_pages, locked_pages, groups); });
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

    const bool has_host_child =
        std::ranges::any_of(node.children | std::views::values, [&](const auto child_id) { return page_subtree_has_host_value(child_id, host_pages); });
    if (node_eligible && !has_host_child) {
        groups.push_back(node.pages);
        return;
    }

    std::ranges::for_each(node.children | std::views::values,
                          [&](const auto child_id) { collect_host_eviction_leaf_groups(child_id, host_pages, evicted_pages, locked_pages, groups); });
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
