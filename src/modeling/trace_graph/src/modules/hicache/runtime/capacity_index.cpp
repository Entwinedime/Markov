/**
 * @file
 * @brief Incremental HiCache capacity and evictable-leaf index implementation.
 */
#include "markov/trace_graph/modules/hicache/runtime/capacity_index.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <set>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace capacity_index_detail {

uint64_t excess(uint64_t occupied, uint64_t capacity) {
    if (capacity == 0 || occupied <= capacity) return 0;
    return occupied - capacity;
}

bool has_host_backup(const HiCacheCacheNode & node) { return node.residency.host_present; }


} // namespace capacity_index_detail

using capacity_index_detail::excess;
using capacity_index_detail::has_host_backup;

bool HiCacheCapacityIndex::VictimKey::operator<(const VictimKey & other) const {
    if (priority != other.priority) return priority < other.priority;
    if (last_access_order != other.last_access_order) return last_access_order < other.last_access_order;
    return node_id < other.node_id;
}

HiCacheCapacityIndex::VictimKey HiCacheCapacityIndex::victim_key(const HiCacheCapacityNodeRecord & record) {
    return VictimKey{
        .priority = record.priority,
        .last_access_order = record.last_access_order,
        .node_id = record.node_id,
    };
}

/** @brief An L1 victim must have no resident descendant, matching SGLang leaf eviction. */
bool HiCacheCapacityIndex::has_device_descendant(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const {
    return std::ranges::any_of(node.children | std::views::values, [&](auto child_id) {
        const auto * child = tree.node(child_id);
        if (child == nullptr) return false;
        if (child->residency.device_present) return true;
        return has_device_descendant(tree, *child);
    });
}

bool HiCacheCapacityIndex::has_backup_child(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const {
    return std::ranges::any_of(node.children | std::views::values, [&](auto child_id) {
        const auto * child = tree.node(child_id);
        if (child == nullptr) return false;
        return has_host_backup(*child);
    });
}

/**
 * @brief Projects one canonical radix node into the incremental capacity index.
 *
 * The projection supports accounting and victim ordering only; canonical residency and
 * references stay on the radix node. An L2 victim must be a host-visible leaf with no
 * lock protection. Host references are checked again at selection time because they can
 * protect a candidate independently of leaf eligibility.
 */
HiCacheCapacityNodeRecord HiCacheCapacityIndex::make_record(const HiCacheTokenRadixTree & tree, HiCacheNodeId node_id) const {
    const auto * node = tree.node(node_id);
    if (node == nullptr || node_id == 0) return HiCacheCapacityNodeRecord{ .node_id = node_id };

    const auto device_leaf = node_id != 0 && node->residency.device_present && node->refs.lock_ref_total == 0 && !has_device_descendant(tree, *node);
    const auto host_leaf = node_id != 0 && !node->residency.device_present && node->residency.host_present && node->residency.host_visible
                           && node->refs.lock_ref_total == 0 && !has_backup_child(tree, *node);
    auto record = HiCacheCapacityNodeRecord{
        .node_id = node_id,
        .active = node->active,
        .page_count = static_cast<uint64_t>(node->pages.size()),
        .priority = node->priority,
        .last_access_order = node->last_access_order,
        .device_present = node->residency.device_present,
        .host_visible = node->residency.host_present && node->residency.host_visible,
        .storage_readable = node->residency.storage_readable,
        .host_ref_total = node->refs.host_ref_total,
        .device_evictable = device_leaf,
        .host_evictable = host_leaf,
    };
    return record;
}

std::set<HiCacheNodeId> HiCacheCapacityIndex::observation_closure(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes) const {
    // Split, insertion, and reference mutations can change ancestor and direct-child
    // eligibility, so incremental synchronization must cover this closure.
    std::set<HiCacheNodeId> nodes;
    auto add_with_ancestors = [&](HiCacheNodeId node_id) {
        nodes.insert(node_id);
        auto current = tree.node(node_id);
        while (current != nullptr) {
            nodes.insert(current->id);
            if (current->id == 0) break;
            current = tree.node(current->parent);
        }
    };
    std::ranges::for_each(seed_nodes, add_with_ancestors);

    std::vector<HiCacheNodeId> with_children{ nodes.begin(), nodes.end() };
    std::ranges::for_each(with_children, [&](auto node_id) {
        const auto * node = tree.node(node_id);
        if (node == nullptr) return;
        std::ranges::copy(node->children | std::views::values, std::inserter(nodes, nodes.end()));
    });
    return nodes;
}

void HiCacheCapacityIndex::remove_record_contribution(const HiCacheCapacityNodeRecord & record) {
    if (!record.active || record.node_id == 0) return;
    if (record.device_present)
        occupied_device_pages_ = core::checked_subtract_u64(occupied_device_pages_, record.page_count, "HiCache L1 capacity index contribution underflow");
    if (record.host_visible)
        occupied_host_pages_ = core::checked_subtract_u64(occupied_host_pages_, record.page_count, "HiCache L2 capacity index contribution underflow");
    if (record.storage_readable)
        readable_storage_pages_ = core::checked_subtract_u64(readable_storage_pages_, record.page_count, "HiCache L3 capacity index contribution underflow");
    if (record.device_evictable) evictable_device_leaves_.erase(victim_key(record));
    if (record.host_evictable) evictable_host_leaves_.erase(victim_key(record));
}

void HiCacheCapacityIndex::add_record_contribution(const HiCacheCapacityNodeRecord & record) {
    if (!record.active || record.node_id == 0) return;
    if (record.device_present)
        occupied_device_pages_ = core::checked_add_u64(occupied_device_pages_, record.page_count, "HiCache L1 capacity index exceeds uint64 range");
    if (record.host_visible)
        occupied_host_pages_ = core::checked_add_u64(occupied_host_pages_, record.page_count, "HiCache L2 capacity index exceeds uint64 range");
    if (record.storage_readable)
        readable_storage_pages_ = core::checked_add_u64(readable_storage_pages_, record.page_count, "HiCache L3 capacity index exceeds uint64 range");
    if (record.device_evictable) evictable_device_leaves_.insert(victim_key(record));
    if (record.host_evictable) evictable_host_leaves_.insert(victim_key(record));
}

void HiCacheCapacityIndex::update_snapshot() {
    snapshot_ = HiCacheCapacitySnapshot{
        .occupied_device_pages = occupied_device_pages_,
        .occupied_host_pages = occupied_host_pages_,
        .readable_storage_pages = readable_storage_pages_,
        .reserved_host_pages = reserved_host_pages_,
    };
}


void HiCacheCapacityIndex::sync_reservation(uint64_t reserved_host_pages, std::string_view reason) {
    // Prefetch reservations do not live in the radix tree but consume L2 capacity. Keep
    // the reservation and residency counters in one allocation-policy snapshot.
    reserved_host_pages_ = reserved_host_pages;
    update_snapshot();
    (void)reason;
}

void HiCacheCapacityIndex::sync_nodes(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes, uint64_t reserved_host_pages,
                                      std::string_view reason) {
    // Every state transition names its affected nodes. Debug audits independently derive
    // the complete index from the tree to catch a missing synchronization closure.
    reserved_host_pages_ = reserved_host_pages;
    (void)reason;

    for (const auto node_id : observation_closure(tree, seed_nodes)) {
        const auto old_it = records_.find(node_id);
        if (old_it != records_.end()) remove_record_contribution(old_it->second);

        auto record = make_record(tree, node_id);
        if (!record.active) {
            records_.erase(node_id);
            continue;
        }

        add_record_contribution(record);
        records_[node_id] = record;
    }

    update_snapshot();
}

uint64_t HiCacheCapacityIndex::device_excess_pages(uint64_t capacity_pages) const { return excess(snapshot_.occupied_device_pages, capacity_pages); }

uint64_t HiCacheCapacityIndex::host_excess_pages(uint64_t capacity_pages) const {
    return excess(core::checked_add_u64(snapshot_.occupied_host_pages, snapshot_.reserved_host_pages, "HiCache committed host capacity exceeds uint64 range"),
                  capacity_pages);
}

std::optional<HiCacheNodeId> HiCacheCapacityIndex::first_device_victim() const {
    if (evictable_device_leaves_.empty()) return std::nullopt;
    return evictable_device_leaves_.begin()->node_id;
}

std::optional<HiCacheNodeId> HiCacheCapacityIndex::first_host_victim() const {
    // A host reference means a request or prefetch can still read the L2 value, so leaf
    // ordering alone cannot authorize eviction.
    for (const auto & candidate : evictable_host_leaves_) {
        const auto record_it = records_.find(candidate.node_id);
        if (record_it == records_.end()) continue;
        if (record_it->second.host_ref_total > 0) continue;
        return candidate.node_id;
    }
    return std::nullopt;
}

std::optional<HiCacheNodeId> HiCacheCapacityIndex::select_device_victim(const HiCacheVictimRequest & request) {
    const auto victim = first_device_victim();
    (void)request;
    return victim;
}

std::optional<HiCacheNodeId> HiCacheCapacityIndex::select_host_victim(const HiCacheVictimRequest & request) {
    const auto victim = first_host_victim();
    (void)request;
    return victim;
}


} // namespace markov::trace_graph::modules::hicache::runtime
