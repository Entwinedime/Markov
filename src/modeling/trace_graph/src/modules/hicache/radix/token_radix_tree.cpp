/**
 * @file
 * @brief Canonical HiCache radix-tree implementation.
 */
#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/radix/node_split_policy.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::radix {

namespace token_radix_tree_detail {

size_t common_prefix_size(const std::vector<std::string> & node_pages, const std::vector<std::string> & path, size_t path_offset = 0) {
    const auto available = path_offset < path.size() ? path.size() - path_offset : 0;
    const auto limit = std::min(node_pages.size(), available);
    size_t matched = 0;
    while (matched < limit && node_pages[matched] == path[path_offset + matched]) ++matched;
    return matched;
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

struct LookupPrefixState {
    bool device_open = true;
    bool host_open = true;
    bool storage_open = true;
    bool visible_open = true;
};

void extend_lookup_prefixes(HiCachePathLookup & result, const HiCacheCacheNode & child, LookupPrefixState & state) {
    if (state.device_open && child.residency.device_present) {
        append_all(result.device_pages, child.pages);
        result.device_chain.push_back(child.id);
        result.deepest_device_node = child.id;
    }
    else state.device_open = false;

    if (state.host_open && child.residency.host_present && child.residency.host_visible) {
        append_all(result.host_pages, child.pages);
        result.host_chain.push_back(child.id);
        result.deepest_host_node = child.id;
    }
    else state.host_open = false;

    if (state.storage_open && child.residency.storage_readable) append_all(result.storage_pages, child.pages);
    else state.storage_open = false;

    const bool visible = child.residency.device_present || (child.residency.host_present && child.residency.host_visible) || child.residency.storage_readable;
    if (state.visible_open && visible) append_all(result.visible_pages, child.pages);
    else state.visible_open = false;
}


} // namespace token_radix_tree_detail

using token_radix_tree_detail::append_all;
using token_radix_tree_detail::common_prefix_size;
using token_radix_tree_detail::extend_lookup_prefixes;
using token_radix_tree_detail::has_host_backup;
using token_radix_tree_detail::LookupPrefixState;
using token_radix_tree_detail::slice_pages;
using token_radix_tree_detail::suffix_pages;

HiCacheTokenRadixTree::HiCacheTokenRadixTree() {
    /**
     * @brief Keeps the topology root free of page residency.
     *
     * The root exists only to anchor page paths for this cache scope; assigning real
     * pages to it would mix topology with residency ownership.
     */
    HiCacheCacheNode root;
    root.id = 0;
    access_clock_ = core::checked_add_u64(access_clock_, 1, "HiCache radix access clock overflow");
    root.last_access_order = access_clock_;
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

const std::vector<std::string> & HiCacheTokenRadixTree::node_pages(HiCacheNodeId node_id) const {
    const auto * current = node(node_id);
    if (current == nullptr) throw std::out_of_range("HiCache radix node is missing or inactive: node_id=" + std::to_string(node_id));
    return current->pages;
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


HiCacheNodeId HiCacheTokenRadixTree::create_child(HiCacheNodeId parent, std::vector<std::string> pages) {
    if (pages.empty()) return parent;
    HiCacheCacheNode child;
    child.id = nodes_.size();
    child.parent = parent;
    child.pages = std::move(pages);
    access_clock_ = core::checked_add_u64(access_clock_, 1, "HiCache radix access clock overflow");
    child.last_access_order = access_clock_;
    nodes_.push_back(std::move(child));
    nodes_[parent].children[nodes_.back().pages.front()] = nodes_.back().id;
    for (const auto & page : nodes_.back().pages) page_to_node_[page] = nodes_.back().id;
    return nodes_.back().id;
}

HiCacheNodeId HiCacheTokenRadixTree::split_child(const ChildSplitRequest & request) {
    /**
     * @brief Copies child residency, references, and hit count into the split prefix.
     *
     * The original child retains the suffix pages, preserving SGLang shared-prefix
     * semantics for both halves of the compressed edge.
     */
    const auto parent = request.parent;
    const auto child_id = request.child;
    const auto split_pages = request.split_pages;
    const auto child_pages = nodes_[child_id].pages;
    if (split_pages == 0 || split_pages >= child_pages.size()) return child_id;

    const auto prefix = slice_pages(child_pages, 0, split_pages);
    const auto suffix = suffix_pages(child_pages, split_pages);

    const HiCacheNodeSplitPolicy policy;
    auto plan = policy.plan(parent,
                            nodes_[child_id],
                            nodes_.size(),
                            HiCacheNodeSplitPages{
                                .prefix = prefix,
                                .suffix = suffix,
                            });
    access_clock_ = core::checked_add_u64(access_clock_, 1, "HiCache radix access clock overflow");
    plan.prefix_node.last_access_order = access_clock_;
    nodes_.push_back(std::move(plan.prefix_node));

    const auto split_id = nodes_.back().id;
    nodes_[parent].children[prefix.front()] = split_id;
    nodes_[split_id].children[suffix.front()] = child_id;
    nodes_[child_id].parent = split_id;
    policy.apply_suffix(nodes_[child_id], plan);
    for (const auto & page : prefix) page_to_node_[page] = split_id;
    for (const auto & page : suffix) page_to_node_[page] = child_id;
    return split_id;
}

HiCacheNodeId HiCacheTokenRadixTree::insert_suffix(HiCacheNodeId parent, const std::vector<std::string> & pages, size_t offset) {
    /**
     * @brief Changes topology without assigning residency.
     *
     * Higher-level device and host insertion operations assign tier residency over
     * the resulting touched chain.
     */
    if (offset >= pages.size()) return parent;
    const auto child_it = nodes_[parent].children.find(pages[offset]);
    if (child_it == nodes_[parent].children.end()) return create_child(parent, slice_pages(pages, offset, pages.size()));

    const auto child_id = child_it->second;
    auto & child = nodes_[child_id];
    const auto shared = common_prefix_size(child.pages, pages, offset);
    if (shared == child.pages.size()) { return offset + shared == pages.size() ? child_id : insert_suffix(child_id, pages, offset + shared); }

    const auto split_id = split_child(ChildSplitRequest{ .parent = parent, .child = child_id, .split_pages = shared });
    return offset + shared == pages.size() ? split_id : create_child(split_id, slice_pages(pages, offset + shared, pages.size()));
}

void HiCacheTokenRadixTree::touch_node(HiCacheNodeId node_id) {
    if (auto * current = mutable_node(node_id); current != nullptr) {
        access_clock_ = core::checked_add_u64(access_clock_, 1, "HiCache radix access clock overflow");
        current->last_access_order = access_clock_;
    }
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
    for (const auto & page : nodes_[node_id].pages) page_to_node_.erase(page);
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
    /**
     * @brief Splits partially matched children while resolving a path.
     *
     * This gives later residency and reference mutations an exact prefix node.
     * `lookup_peek` suppresses access refresh but may still canonicalize topology.
     */
    HiCachePathLookup result;
    if (pages.empty()) return result;

    HiCacheNodeId current_id = 0;
    size_t matched = 0;
    LookupPrefixState prefix_state;

    while (matched < pages.size()) {
        auto & current = nodes_[current_id];
        const auto child_it = current.children.find(pages[matched]);
        if (child_it == current.children.end()) break;
        auto child_id = child_it->second;
        const auto shared = common_prefix_size(nodes_[child_id].pages, pages, matched);
        if (shared == 0) break;
        const bool split = shared < nodes_[child_id].pages.size();
        if (split) {
            if (refresh_access) touch_node(child_id);
            child_id = split_child(ChildSplitRequest{ .parent = current_id, .child = child_id, .split_pages = shared });
        }
        const auto & child_pages = nodes_[child_id].pages;

        if (refresh_access && !split) touch_node(child_id);
        append_all(result.topology_pages, child_pages);
        result.topology_chain.push_back(child_id);
        result.terminal_node = child_id;

        const auto & child = nodes_[child_id];
        extend_lookup_prefixes(result, child, prefix_state);

        matched += child_pages.size();
        current_id = child_id;
    }
    return result;
}

std::vector<std::string> HiCacheTokenRadixTree::contiguous_prefix(const std::vector<std::string> & pages, bool include_device, bool include_host,
                                                                  bool include_storage) {
    /**
     * @brief Requires visibility to remain contiguous from the first page.
     *
     * A gap at any tier terminates the prefix; later visible pages cannot be skipped to.
     */
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
    /**
     * @brief Marks device residency along the complete terminal chain.
     *
     * A node with a host backup is restored rather than newly produced and therefore
     * does not become dirty solely because of this insertion.
     */
    HiCacheInsertResult result;
    if (pages.empty()) return result;
    auto existing = lookup(pages);
    result.existing_device_prefix_pages = static_cast<uint64_t>(existing.device_pages.size());
    result.existing_topology_prefix_pages = static_cast<uint64_t>(existing.topology_pages.size());
    result.inserted_key_pages = static_cast<uint64_t>(pages.size());
    result.page_aligned_key_pages = static_cast<uint64_t>(pages.size());
    const auto existing_node_count = nodes_.size();

    result.terminal_node = insert_suffix(0, pages, 0);
    result.touched_nodes = ancestor_node_ids(result.terminal_node);
    for (const auto node_id : result.touched_nodes) {
        auto & current = nodes_[node_id];
        current.priority = std::max(current.priority, priority);
        touch_node(node_id);
        if (current.residency.device_present) continue;
        const bool existed = node_id < existing_node_count;
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
    /**
     * @brief Allows host insertion to materialize only the visible prefix.
     *
     * Splits may create topology beyond `visible_pages`; those nodes must not gain
     * host visibility.
     */
    HiCacheInsertResult result;
    if (pages.empty()) return result;
    auto existing = lookup(pages);
    result.existing_device_prefix_pages = static_cast<uint64_t>(existing.device_pages.size());
    result.existing_topology_prefix_pages = static_cast<uint64_t>(existing.topology_pages.size());
    result.inserted_key_pages = static_cast<uint64_t>(pages.size());
    result.page_aligned_key_pages = static_cast<uint64_t>(pages.size());
    result.terminal_node = insert_suffix(0, pages, 0);
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
            const auto owner_ref = current->refs.lock_refs_by_owner.find(owner);
            const auto owner_count = owner_ref == current->refs.lock_refs_by_owner.end() ? uint64_t{ 0 } : owner_ref->second;
            const auto next_total = core::checked_add_u64(current->refs.lock_ref_total, 1, "HiCache lock-ref total overflow");
            const auto next_owner = core::checked_add_u64(owner_count, 1, "HiCache lock-ref owner count overflow");
            current->refs.lock_ref_total = next_total;
            current->refs.lock_refs_by_owner.insert_or_assign(owner, next_owner);
        }
    }
}

void HiCacheTokenRadixTree::add_host_ref(const std::vector<HiCacheNodeId> & chain, const std::string & owner) {
    if (owner.empty()) return;
    for (const auto node_id : chain) {
        if (auto * current = mutable_node(node_id); current != nullptr) {
            const auto owner_ref = current->refs.host_refs_by_owner.find(owner);
            const auto owner_count = owner_ref == current->refs.host_refs_by_owner.end() ? uint64_t{ 0 } : owner_ref->second;
            const auto next_total = core::checked_add_u64(current->refs.host_ref_total, 1, "HiCache host-ref total overflow");
            const auto next_owner = core::checked_add_u64(owner_count, 1, "HiCache host-ref owner count overflow");
            current->refs.host_ref_total = next_total;
            current->refs.host_refs_by_owner.insert_or_assign(owner, next_owner);
        }
    }
}

void HiCacheTokenRadixTree::release_refs_by_owner(const std::string & owner) {
    /**
     * @brief Releases reference counters atomically by owner identity.
     *
     * The tree and ref ledger are one invariant boundary. A repeated release or
     * mismatched owner count is a model error and must not be hidden by saturation.
     */
    if (owner.empty()) return;
    for (auto & current : nodes_) {
        if (!current.active) continue;
        if (const auto it = current.refs.lock_refs_by_owner.find(owner); it != current.refs.lock_refs_by_owner.end()) {
            current.refs.lock_ref_total = core::checked_subtract_u64(current.refs.lock_ref_total, it->second, "HiCache lock-ref release underflow");
            current.refs.lock_refs_by_owner.erase(it);
        }
        if (const auto it = current.refs.host_refs_by_owner.find(owner); it != current.refs.host_refs_by_owner.end()) {
            current.refs.host_ref_total = core::checked_subtract_u64(current.refs.host_ref_total, it->second, "HiCache host-ref release underflow");
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
    /**
     * @brief Supports write-back demotion before releasing device residency.
     *
     * Ordinary device eviction never invents a host backup.
     */
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

HiCacheDeviceEvictionResult HiCacheTokenRadixTree::evict_unbacked_device_leaf(HiCacheNodeId node_id) {
    auto result = HiCacheDeviceEvictionResult{
        .node_id = node_id,
        .pages = {},
        .affected_nodes = {},
        .reason = {},
    };
    auto * current = mutable_node(node_id);
    if (current == nullptr || node_id == 0) {
        result.reason = "missing_or_root_node";
        result.affected_nodes.push_back(node_id);
        return result;
    }
    result.parent_node = current->parent;
    result.pages = current->pages;
    result.affected_nodes = { current->parent };
    if (!current->residency.device_present) {
        result.reason = "device_leaf_has_no_device_value";
        return result;
    }
    if (current->residency.host_present) {
        result.reason = "device_leaf_has_host_backup";
        return result;
    }
    if (current->refs.lock_ref_total > 0 || current->refs.host_ref_total > 0) {
        result.reason = "device_leaf_reference_protected";
        return result;
    }
    if (!current->children.empty()) {
        result.reason = "device_leaf_has_active_child";
        return result;
    }

    auto * parent = mutable_node(current->parent);
    if (parent == nullptr || current->pages.empty()) {
        result.reason = "device_leaf_missing_parent_or_key";
        return result;
    }
    const auto erased = parent->children.erase(current->pages.front());
    if (erased == 0) {
        result.reason = "device_leaf_parent_child_missing";
        return result;
    }
    deactivate_subtree(node_id, result.affected_nodes);
    result.evicted = true;
    result.reason = "evict_unbacked_device_leaf";
    return result;
}

HiCacheHostEvictionResult HiCacheTokenRadixTree::evict_host_leaf(HiCacheNodeId node_id) {
    /**
     * @brief Removes the host-visible leaf or subtree rather than clearing one flag.
     *
     * Device residency, references, or backed-up children prevent eviction so the
     * remaining radix topology stays explainable.
     */
    auto result = HiCacheHostEvictionResult{
        .node_id = node_id,
        .pages = {},
        .affected_nodes = {},
        .reason = {},
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
    result.evicted = true;
    result.reason = "evict_host_leaf";
    return result;
}

} // namespace markov::trace_graph::modules::hicache::radix
