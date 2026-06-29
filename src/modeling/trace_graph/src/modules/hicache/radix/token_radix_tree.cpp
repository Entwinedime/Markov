/**
 * @file
 * @brief HiCache canonical radix tree 实现。
 */
#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"

#include "markov/trace_graph/modules/hicache/radix/node_split_policy.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <set>
#include <utility>

namespace markov::trace_graph::modules::hicache::radix {

namespace token_radix_tree_detail {

size_t common_prefix_size(const std::vector<std::string> & left, const std::vector<std::string> & right) {
    const auto [left_it, right_it] = std::ranges::mismatch(left, right);
    (void)right_it;
    return static_cast<size_t>(std::ranges::distance(left.begin(), left_it));
}

std::vector<std::string> slice_pages(const std::vector<std::string> & pages, size_t begin, size_t end) {
    if (begin >= end || begin >= pages.size()) return {};
    end = std::min(end, pages.size());
    std::vector<std::string> result;
    result.reserve(end - begin);
    auto view = pages | std::views::drop(static_cast<std::ranges::range_difference_t<std::vector<std::string>>>(begin))
                | std::views::take(static_cast<std::ranges::range_difference_t<std::vector<std::string>>>(end - begin));
    std::ranges::copy(view, std::back_inserter(result));
    return result;
}

std::vector<std::string> suffix_pages(const std::vector<std::string> & pages, size_t begin) { return slice_pages(pages, begin, pages.size()); }

void append_all(std::vector<std::string> & target, const std::vector<std::string> & source) { target.insert(target.end(), source.begin(), source.end()); }

bool has_host_backup(const HiCacheCacheNode & node) { return node.residency.host_present; }

std::string page_hash_from_id(const std::string & page_id) {
    const auto delimiter = page_id.find('|');
    if (delimiter == std::string::npos) return page_id;
    return page_id.substr(delimiter + 1);
}

std::string page_scope_from_id(const std::string & page_id) {
    const auto delimiter = page_id.find('|');
    if (delimiter == std::string::npos) return "-1";
    return page_id.substr(0, delimiter);
}

} // namespace token_radix_tree_detail

using token_radix_tree_detail::append_all;
using token_radix_tree_detail::common_prefix_size;
using token_radix_tree_detail::has_host_backup;
using token_radix_tree_detail::page_hash_from_id;
using token_radix_tree_detail::page_scope_from_id;
using token_radix_tree_detail::slice_pages;
using token_radix_tree_detail::suffix_pages;

HiCacheTokenRadixTree::HiCacheTokenRadixTree() {
    /* root 不承载 page residency，仅作为所有 cache_scope/page path 的拓扑根。 */
    HiCacheCacheNode root;
    root.id = 0;
    root.last_access_order = ++access_clock_;
    nodes_.push_back(std::move(root));
}

const HiCacheCacheNode * HiCacheTokenRadixTree::node(HiCacheNodeId node_id) const {
    if (node_id >= nodes_.size() || !nodes_[node_id].active) return nullptr;
    return &nodes_[node_id];
}

HiCacheCacheNode * HiCacheTokenRadixTree::mutable_node(HiCacheNodeId node_id) {
    if (node_id >= nodes_.size() || !nodes_[node_id].active) return nullptr;
    return &nodes_[node_id];
}

bool HiCacheTokenRadixTree::contains_page(const std::string & page) const { return page_to_node_.contains(page); }

std::optional<HiCacheNodeId> HiCacheTokenRadixTree::node_for_page(const std::string & page) const {
    const auto it = page_to_node_.find(page);
    if (it == page_to_node_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> HiCacheTokenRadixTree::node_pages(HiCacheNodeId node_id) const {
    const auto * current = node(node_id);
    return current == nullptr ? std::vector<std::string>{} : current->pages;
}

std::vector<HiCacheNodeId> HiCacheTokenRadixTree::ancestor_node_ids(HiCacheNodeId terminal_node) const {
    std::vector<HiCacheNodeId> chain;
    auto current = node(terminal_node);
    while (current != nullptr && current->id != 0) {
        chain.push_back(current->id);
        current = node(current->parent);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

std::vector<std::string> HiCacheTokenRadixTree::flattened_pages(HiCacheNodeId terminal_node) const {
    std::vector<std::string> pages;
    for (const auto node_id : ancestor_node_ids(terminal_node)) append_all(pages, nodes_[node_id].pages);
    return pages;
}

void HiCacheTokenRadixTree::observe_page_path(const HiCachePagePath & path) {
    /* projection metadata 只服务 split diagnostics 和 storage key 还原；真正 residency
       状态仍保存在 radix node 上。 */
    std::ranges::for_each(path.pages, [&](const auto & page) { page_projection_[page.id] = page; });
}

HiCacheNodeSplitProjection HiCacheTokenRadixTree::split_projection(const std::vector<std::string> & pages, uint64_t depth_page_begin) const {
    /* split 发生后 prefix/suffix 仍需能解释到原 token/page 区间。缺 projection
       metadata 时继续保留 page hash，但把 token_span_known 标成 false。 */
    HiCacheNodeSplitProjection projection{
        .depth_page_begin = depth_page_begin,
        .depth_page_end = depth_page_begin + static_cast<uint64_t>(pages.size()),
    };
    projection.page_hashes.reserve(pages.size());
    projection.storage_keys.reserve(pages.size());

    std::optional<uint64_t> token_begin;
    std::optional<uint64_t> token_end;
    bool all_token_spans_known = !pages.empty();
    for (const auto & page : pages) {
        if (const auto it = page_projection_.find(page); it != page_projection_.end()) {
            projection.page_hashes.push_back(it->second.hash.empty() ? page_hash_from_id(page) : it->second.hash);
            projection.storage_keys.push_back(it->second.cache_scope + "|" + (it->second.hash.empty() ? page_hash_from_id(page) : it->second.hash));
            token_begin =
                token_begin ? std::optional<uint64_t>{ std::min(*token_begin, it->second.token_begin) } : std::optional<uint64_t>{ it->second.token_begin };
            token_end = token_end ? std::optional<uint64_t>{ std::max(*token_end, it->second.token_end) } : std::optional<uint64_t>{ it->second.token_end };
            all_token_spans_known = all_token_spans_known && it->second.token_end > it->second.token_begin;
            continue;
        }
        const auto scope = page_scope_from_id(page);
        const auto hash = page_hash_from_id(page);
        projection.page_hashes.push_back(hash);
        projection.storage_keys.push_back(scope + "|" + hash);
        all_token_spans_known = false;
    }
    projection.token_span_known = all_token_spans_known && token_begin.has_value() && token_end.has_value();
    if (projection.token_span_known) {
        projection.token_begin = *token_begin;
        projection.token_end = *token_end;
    }
    return projection;
}

HiCacheNodeId HiCacheTokenRadixTree::create_child(HiCacheNodeId parent, std::vector<std::string> pages) {
    if (pages.empty()) return parent;
    HiCacheCacheNode child;
    child.id = nodes_.size();
    child.parent = parent;
    child.pages = std::move(pages);
    child.last_access_order = ++access_clock_;
    nodes_.push_back(std::move(child));
    nodes_[parent].children[nodes_.back().pages.front()] = nodes_.back().id;
    return nodes_.back().id;
}

HiCacheNodeId HiCacheTokenRadixTree::split_child(HiCacheNodeId parent, HiCacheNodeId child_id, size_t split_pages) {
    /* radix split 会把旧 child 的 residency/ref/hit_count 复制到 prefix node，
       suffix node 再承接剩余 pages。这模拟 SGLang radix cache 中共享前缀节点的语义。 */
    const auto child_pages = nodes_[child_id].pages;
    if (split_pages == 0 || split_pages >= child_pages.size()) return child_id;

    const auto prefix = slice_pages(child_pages, 0, split_pages);
    const auto suffix = suffix_pages(child_pages, split_pages);
    const auto parent_depth_pages = static_cast<uint64_t>(flattened_pages(parent).size());
    const auto context = HiCacheNodeSplitContext{
        .parent_child_key = prefix.front(),
        .suffix_child_key = suffix.front(),
        .prefix_projection = split_projection(prefix, parent_depth_pages),
        .suffix_projection = split_projection(suffix, parent_depth_pages + static_cast<uint64_t>(prefix.size())),
    };

    const HiCacheNodeSplitPolicy policy;
    auto plan = policy.plan(parent, nodes_[child_id], nodes_.size(), split_pages, prefix, suffix, context);
    nodes_.push_back(std::move(plan.prefix_node));

    const auto split_id = nodes_.back().id;
    nodes_[parent].children[prefix.front()] = split_id;
    nodes_[split_id].children[suffix.front()] = child_id;
    nodes_[child_id].parent = split_id;
    policy.apply_suffix(nodes_[child_id], plan);
    split_history_.push_back(std::move(plan.record));
    rebuild_page_index();
    return split_id;
}

HiCacheNodeId HiCacheTokenRadixTree::insert_suffix(HiCacheNodeId parent, const std::vector<std::string> & suffix) {
    /* 插入只改变拓扑；L1/L2/L3 residency 由 insert_device_path/insert_host_path 等
       higher-level 操作在 touched chain 上赋值。 */
    if (suffix.empty()) return parent;
    const auto child_it = nodes_[parent].children.find(suffix.front());
    if (child_it == nodes_[parent].children.end()) return create_child(parent, suffix);

    const auto child_id = child_it->second;
    auto & child = nodes_[child_id];
    const auto shared = common_prefix_size(child.pages, suffix);
    if (shared == child.pages.size()) {
        const auto remaining = suffix_pages(suffix, shared);
        return remaining.empty() ? child_id : insert_suffix(child_id, remaining);
    }

    const auto split_id = split_child(parent, child_id, shared);
    const auto remaining = suffix_pages(suffix, shared);
    return remaining.empty() ? split_id : create_child(split_id, remaining);
}

void HiCacheTokenRadixTree::rebuild_page_index() {
    page_to_node_.clear();
    for (const auto & current : nodes_) {
        if (!current.active || current.id == 0) continue;
        std::ranges::for_each(current.pages, [&](const auto & page) { page_to_node_[page] = current.id; });
    }
}

void HiCacheTokenRadixTree::touch_node(HiCacheNodeId node_id) {
    if (auto * current = mutable_node(node_id); current != nullptr) current->last_access_order = ++access_clock_;
}

bool HiCacheTokenRadixTree::has_backup_child(HiCacheNodeId node_id) const {
    const auto * current = node(node_id);
    if (current == nullptr) return false;
    return std::ranges::any_of(current->children | std::views::values, [&](auto child_id) {
        const auto * child = node(child_id);
        return child != nullptr && has_host_backup(*child);
    });
}

void HiCacheTokenRadixTree::deactivate_subtree(HiCacheNodeId node_id, std::vector<HiCacheNodeId> & affected_nodes) {
    if (node_id >= nodes_.size() || !nodes_[node_id].active) return;
    auto child_ids = std::vector<HiCacheNodeId>{};
    child_ids.reserve(nodes_[node_id].children.size());
    std::ranges::copy(nodes_[node_id].children | std::views::values, std::back_inserter(child_ids));
    std::ranges::for_each(child_ids, [&](auto child_id) { deactivate_subtree(child_id, affected_nodes); });
    nodes_[node_id].children.clear();
    nodes_[node_id].refs = HiCacheNodeRefState{};
    nodes_[node_id].residency = HiCacheNodeResidency{};
    nodes_[node_id].active = false;
    affected_nodes.push_back(node_id);
}

void HiCacheTokenRadixTree::touch_chain(const std::vector<HiCacheNodeId> & chain) {
    std::ranges::for_each(chain, [&](auto node_id) { touch_node(node_id); });
}

HiCachePathLookup HiCacheTokenRadixTree::lookup(const std::vector<std::string> & pages) { return lookup_impl(pages, true); }

HiCachePathLookup HiCacheTokenRadixTree::lookup_peek(const std::vector<std::string> & pages) { return lookup_impl(pages, false); }

HiCachePathLookup HiCacheTokenRadixTree::lookup_impl(const std::vector<std::string> & pages, bool refresh_access) {
    /* lookup 会按需 split 部分命中的 child，使后续 residency/ref 变更能落在精确
       prefix node 上。lookup_peek 关闭 access refresh，但仍可能规范化拓扑。 */
    HiCachePathLookup result;
    if (pages.empty()) return result;

    HiCacheNodeId current_id = 0;
    size_t matched = 0;
    bool device_prefix_open = true;
    bool host_prefix_open = true;
    bool storage_prefix_open = true;
    bool visible_prefix_open = true;

    while (matched < pages.size()) {
        auto & current = nodes_[current_id];
        const auto child_it = current.children.find(pages[matched]);
        if (child_it == current.children.end()) break;
        auto child_id = child_it->second;
        auto child_pages = nodes_[child_id].pages;
        auto remaining = suffix_pages(pages, matched);
        const auto shared = common_prefix_size(child_pages, remaining);
        if (shared == 0) break;
        if (shared < child_pages.size()) {
            child_id = split_child(current_id, child_id, shared);
            child_pages = nodes_[child_id].pages;
        }

        if (refresh_access) touch_node(child_id);
        append_all(result.topology_pages, child_pages);
        result.topology_chain.push_back(child_id);
        result.terminal_node = child_id;

        const auto & child = nodes_[child_id];
        if (device_prefix_open && child.residency.device_present) {
            append_all(result.device_pages, child.pages);
            result.device_chain.push_back(child_id);
            result.deepest_device_node = child_id;
        }
        else { device_prefix_open = false; }

        if (host_prefix_open && child.residency.host_present && child.residency.host_visible) {
            append_all(result.host_pages, child.pages);
            result.host_chain.push_back(child_id);
            result.deepest_host_node = child_id;
        }
        else { host_prefix_open = false; }

        if (storage_prefix_open && child.residency.storage_readable) append_all(result.storage_pages, child.pages);
        else storage_prefix_open = false;

        const auto visible =
            child.residency.device_present || (child.residency.host_present && child.residency.host_visible) || child.residency.storage_readable;
        if (visible_prefix_open && visible) append_all(result.visible_pages, child.pages);
        else visible_prefix_open = false;

        matched += child_pages.size();
        current_id = child_id;
    }
    return result;
}

std::vector<std::string> HiCacheTokenRadixTree::contiguous_prefix(const std::vector<std::string> & pages, bool include_device, bool include_host,
                                                                  bool include_storage) {
    /* prefix 查询要求从第一页开始连续可见；任一层级断开后不能跳过缺口继续命中。 */
    (void)lookup_peek(pages);
    std::vector<std::string> prefix;
    prefix.reserve(pages.size());
    for (const auto & page : pages) {
        const auto owner = node_for_page(page);
        if (!owner) break;
        const auto & current = nodes_[*owner];
        const bool visible = (include_device && current.residency.device_present)
                             || (include_host && current.residency.host_present && current.residency.host_visible)
                             || (include_storage && current.residency.storage_readable);
        if (!visible) break;
        prefix.push_back(page);
    }
    return prefix;
}

HiCacheInsertResult HiCacheTokenRadixTree::insert_device_path(const std::vector<std::string> & pages, int64_t priority, bool dirty) {
    /* L1 insert 会沿 terminal chain 标记 device_present。若节点已有 host backup，
       该 device value 视为从 backup 恢复，不再因为本次 insert 变成 dirty。 */
    HiCacheInsertResult result;
    if (pages.empty()) return result;
    auto existing = lookup(pages);
    result.existing_device_prefix_pages = static_cast<uint64_t>(existing.device_pages.size());
    result.existing_topology_prefix_pages = static_cast<uint64_t>(existing.topology_pages.size());
    result.inserted_key_pages = static_cast<uint64_t>(pages.size());
    result.page_aligned_key_pages = static_cast<uint64_t>(pages.size());
    std::set<HiCacheNodeId> existing_nodes;
    std::ranges::for_each(nodes_, [&](const auto & current) {
        if (current.active) existing_nodes.insert(current.id);
    });

    result.terminal_node = insert_suffix(0, pages);
    rebuild_page_index();
    result.touched_nodes = ancestor_node_ids(result.terminal_node);
    for (const auto node_id : result.touched_nodes) {
        auto & current = nodes_[node_id];
        current.priority = std::max(current.priority, priority);
        touch_node(node_id);
        if (current.residency.device_present) continue;
        const bool existed = existing_nodes.contains(node_id);
        const bool had_backup = has_host_backup(current);
        const bool was_dirty = current.residency.device_dirty;
        current.residency.device_present = true;
        current.residency.device_dirty = dirty && !had_backup;
        if (!was_dirty && current.residency.device_dirty) result.dirtied_device_nodes.push_back(node_id);
        if (existed && had_backup) result.restored_device_nodes.push_back(node_id);
        else result.new_device_nodes.push_back(node_id);
    }
    return result;
}

HiCacheInsertResult HiCacheTokenRadixTree::insert_host_path(const std::vector<std::string> & pages, bool storage_readable) {
    return insert_host_path(pages, std::set<std::string>(pages.begin(), pages.end()), storage_readable);
}

HiCacheInsertResult HiCacheTokenRadixTree::insert_host_path(const std::vector<std::string> & pages, const std::set<std::string> & visible_pages,
                                                            bool storage_readable) {
    /* Host insertion 可以只 materialize 可见 prefix。visible_pages 之外的 topology
       node 可能已因 split/insert 创建，但不能被标成 host_visible。 */
    HiCacheInsertResult result;
    if (pages.empty()) return result;
    auto existing = lookup(pages);
    result.existing_device_prefix_pages = static_cast<uint64_t>(existing.device_pages.size());
    result.existing_topology_prefix_pages = static_cast<uint64_t>(existing.topology_pages.size());
    result.inserted_key_pages = static_cast<uint64_t>(pages.size());
    result.page_aligned_key_pages = static_cast<uint64_t>(pages.size());
    result.terminal_node = insert_suffix(0, pages);
    rebuild_page_index();
    result.touched_nodes = ancestor_node_ids(result.terminal_node);
    for (const auto node_id : result.touched_nodes) {
        auto & current = nodes_[node_id];
        if (!std::ranges::all_of(current.pages, [&](const auto & page) { return visible_pages.contains(page); })) continue;
        touch_node(node_id);
        const bool new_host = !current.residency.host_present || !current.residency.host_visible;
        current.residency.host_present = true;
        current.residency.host_visible = true;
        current.residency.storage_known = true;
        current.residency.storage_readable = current.residency.storage_readable || storage_readable;
        if (new_host) result.new_host_nodes.push_back(node_id);
    }
    return result;
}

void HiCacheTokenRadixTree::add_lock_ref(const std::vector<HiCacheNodeId> & chain, const std::string & owner) {
    if (owner.empty()) return;
    for (const auto node_id : chain) {
        if (auto * current = mutable_node(node_id); current != nullptr) {
            current->refs.lock_ref_total++;
            current->refs.lock_refs_by_owner[owner]++;
        }
    }
}

void HiCacheTokenRadixTree::add_host_ref(const std::vector<HiCacheNodeId> & chain, const std::string & owner) {
    if (owner.empty()) return;
    for (const auto node_id : chain) {
        if (auto * current = mutable_node(node_id); current != nullptr) {
            current->refs.host_ref_total++;
            current->refs.host_refs_by_owner[owner]++;
        }
    }
}

void HiCacheTokenRadixTree::release_refs_by_owner(const std::string & owner) {
    /* ref counter 以 owner 为删除单位，和 HiCacheRefLedger::release_owner 保持同一
       语义；计数用 saturating subtract 防止诊断路径重复释放造成 underflow。 */
    if (owner.empty()) return;
    for (auto & current : nodes_) {
        if (!current.active) continue;
        if (const auto it = current.refs.lock_refs_by_owner.find(owner); it != current.refs.lock_refs_by_owner.end()) {
            current.refs.lock_ref_total -= std::min(current.refs.lock_ref_total, it->second);
            current.refs.lock_refs_by_owner.erase(it);
        }
        if (const auto it = current.refs.host_refs_by_owner.find(owner); it != current.refs.host_refs_by_owner.end()) {
            current.refs.host_ref_total -= std::min(current.refs.host_ref_total, it->second);
            current.refs.host_refs_by_owner.erase(it);
        }
    }
}

void HiCacheTokenRadixTree::mark_host_visible(HiCacheNodeId node_id, bool storage_readable) {
    if (auto * current = mutable_node(node_id); current != nullptr) {
        current->residency.host_present = true;
        current->residency.host_visible = true;
        current->residency.storage_known = true;
        current->residency.storage_readable = current->residency.storage_readable || storage_readable;
    }
}

void HiCacheTokenRadixTree::clear_dirty(HiCacheNodeId node_id) {
    if (auto * current = mutable_node(node_id); current != nullptr) current->residency.device_dirty = false;
}

void HiCacheTokenRadixTree::demote_device_to_host(HiCacheNodeId node_id, bool ensure_host) {
    /* write-back eviction 可以把 dirty device 同步为 host/storage-readable 后再释放
       L1；普通 device eviction 则不会凭空创建 host backup。 */
    if (auto * current = mutable_node(node_id); current != nullptr) {
        if (ensure_host) {
            current->residency.host_present = true;
            current->residency.host_visible = true;
            current->residency.storage_known = true;
            current->residency.storage_readable = true;
        }
        current->residency.device_present = false;
        current->residency.device_dirty = false;
    }
}

void HiCacheTokenRadixTree::remove_device_regular(HiCacheNodeId node_id) {
    if (auto * current = mutable_node(node_id); current != nullptr) {
        current->residency.device_present = false;
        current->residency.device_dirty = false;
    }
}

HiCacheHostEvictionResult HiCacheTokenRadixTree::evict_host_leaf(HiCacheNodeId node_id) {
    /* L2 cleanup 删除的是 host-visible leaf/subtree，而不是简单清掉一个 flag。
       有 device value、ref 或 backup child 时都不能驱逐，保持 radix topology 可解释。 */
    auto result = HiCacheHostEvictionResult{
        .node_id = node_id,
    };
    auto * current = mutable_node(node_id);
    if (current == nullptr || node_id == 0) {
        result.reason = "missing_or_root_node";
        result.affected_nodes.push_back(node_id);
        return result;
    }

    result.parent_node = current->parent;
    result.pages = current->pages;
    result.affected_nodes = { node_id, current->parent };
    if (current->residency.device_present) {
        result.reason = "host_leaf_still_has_device_value";
        return result;
    }
    if (!current->residency.host_present) {
        result.reason = "host_leaf_has_no_host_value";
        return result;
    }
    if (current->refs.lock_ref_total > 0) {
        result.reason = "host_leaf_lock_ref_protected";
        return result;
    }
    if (current->refs.host_ref_total > 0) {
        result.reason = "host_leaf_host_ref_protected";
        return result;
    }
    if (has_backup_child(node_id)) {
        result.reason = "host_leaf_has_backup_child";
        return result;
    }

    auto * parent = mutable_node(current->parent);
    if (parent == nullptr || current->pages.empty()) {
        result.reason = "host_leaf_missing_parent_or_key";
        return result;
    }
    const auto erased = parent->children.erase(current->pages.front());
    if (erased == 0) {
        result.reason = "host_leaf_parent_child_missing";
        return result;
    }

    result.affected_nodes.clear();
    result.affected_nodes.push_back(parent->id);
    deactivate_subtree(node_id, result.affected_nodes);
    rebuild_page_index();
    result.evicted = true;
    result.reason = "evict_host_leaf";
    return result;
}

} // namespace markov::trace_graph::modules::hicache::radix
