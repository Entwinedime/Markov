/**
 * @file
 * @brief HiCache capacity leaf index。
 */
#include "trace_graph/modules/hicache/hicache_capacity_index.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <set>
#include <utility>

namespace TraceGraph {

namespace {

uint64_t excess(uint64_t occupied, uint64_t capacity) {
    if (capacity == 0 || occupied <= capacity) return 0;
    return occupied - capacity;
}

bool has_backup(const HiCacheCacheNode & node) { return node.residency.host_present || node.residency.storage_readable; }

bool same_residency(const HiCacheNodeResidency & left, const HiCacheNodeResidency & right) {
    return left.device_present == right.device_present && left.device_dirty == right.device_dirty && left.host_present == right.host_present
           && left.host_visible == right.host_visible && left.storage_known == right.storage_known && left.storage_readable == right.storage_readable;
}

bool same_refs(const HiCacheNodeRefState & left, const HiCacheNodeRefState & right) {
    return left.lock_ref_total == right.lock_ref_total && left.host_ref_total == right.host_ref_total && left.lock_refs_by_owner == right.lock_refs_by_owner
           && left.host_refs_by_owner == right.host_refs_by_owner;
}

} // namespace

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

bool HiCacheCapacityIndex::has_device_descendant(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const {
    return std::ranges::any_of(node.children | std::views::values, [&](auto child_id) {
        const auto * child = tree.node(child_id);
        if (child == nullptr) return false;
        if (child->residency.device_present) return true;
        return has_device_descendant(tree, *child);
    });
}

bool HiCacheCapacityIndex::has_backup_descendant(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const {
    return std::ranges::any_of(node.children | std::views::values, [&](auto child_id) {
        const auto * child = tree.node(child_id);
        if (child == nullptr) return false;
        if (has_backup(*child)) return true;
        return has_backup_descendant(tree, *child);
    });
}

HiCacheCapacityNodeRecord HiCacheCapacityIndex::make_record(const HiCacheTokenRadixTree & tree, HiCacheNodeId node_id) const {
    const auto * node = tree.node(node_id);
    if (node == nullptr) return HiCacheCapacityNodeRecord{ .node_id = node_id };

    const auto device_leaf = node_id != 0 && node->residency.device_present && node->refs.lock_ref_total == 0 && !has_device_descendant(tree, *node);
    const auto host_leaf = node_id != 0 && !node->residency.device_present && node->residency.host_present && node->residency.host_visible
                           && node->refs.lock_ref_total == 0 && node->refs.host_ref_total == 0 && !has_backup_descendant(tree, *node);
    return HiCacheCapacityNodeRecord{
        .node_id = node_id,
        .parent = node->parent,
        .active = node->active,
        .page_count = static_cast<uint64_t>(node->pages.size()),
        .pages = node->pages,
        .priority = node->priority,
        .last_access_order = node->last_access_order,
        .residency = node->residency,
        .refs = node->refs,
        .device_evictable = device_leaf,
        .host_evictable = host_leaf,
        .observed_epoch = mutation_epoch_,
    };
}

std::set<HiCacheNodeId> HiCacheCapacityIndex::observation_closure(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes) const {
    std::set<HiCacheNodeId> nodes;
    auto add_with_ancestors = [&](HiCacheNodeId node_id) {
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
    if (record.residency.device_present) occupied_device_pages_ -= std::min(occupied_device_pages_, record.page_count);
    if (record.residency.host_present && record.residency.host_visible) occupied_host_pages_ -= std::min(occupied_host_pages_, record.page_count);
    if (record.residency.storage_readable) readable_storage_pages_ -= std::min(readable_storage_pages_, record.page_count);
    if (record.device_evictable) evictable_device_leaves_.erase(victim_key(record));
    if (record.host_evictable) evictable_host_leaves_.erase(victim_key(record));
}

void HiCacheCapacityIndex::add_record_contribution(const HiCacheCapacityNodeRecord & record) {
    if (!record.active || record.node_id == 0) return;
    if (record.residency.device_present) occupied_device_pages_ += record.page_count;
    if (record.residency.host_present && record.residency.host_visible) occupied_host_pages_ += record.page_count;
    if (record.residency.storage_readable) readable_storage_pages_ += record.page_count;
    if (record.device_evictable) evictable_device_leaves_.insert(victim_key(record));
    if (record.host_evictable) evictable_host_leaves_.insert(victim_key(record));
}

void HiCacheCapacityIndex::update_snapshot() {
    auto device_leaves = std::vector<HiCacheNodeId>{};
    device_leaves.reserve(evictable_device_leaves_.size());
    std::ranges::transform(evictable_device_leaves_, std::back_inserter(device_leaves), &VictimKey::node_id);

    auto host_leaves = std::vector<HiCacheNodeId>{};
    host_leaves.reserve(evictable_host_leaves_.size());
    std::ranges::transform(evictable_host_leaves_, std::back_inserter(host_leaves), &VictimKey::node_id);

    snapshot_ = HiCacheCapacitySnapshot{
        .occupied_device_pages = occupied_device_pages_,
        .occupied_host_pages = occupied_host_pages_,
        .readable_storage_pages = readable_storage_pages_,
        .reserved_host_pages = reserved_host_pages_,
        .evictable_device_leaves = std::move(device_leaves),
        .evictable_host_leaves = std::move(host_leaves),
    };
}

void HiCacheCapacityIndex::record_mutation(HiCacheCapacityMutation mutation) { mutation_trace_.push_back(std::move(mutation)); }

std::optional<HiCacheCapacityNodeRecord> HiCacheCapacityIndex::indexed_record(HiCacheNodeId node_id) const {
    const auto it = records_.find(node_id);
    if (it == records_.end()) return std::nullopt;
    return it->second;
}

HiCacheCapacityVictimChoice HiCacheCapacityIndex::make_victim_choice(const std::string & tier, const std::string & reason, uint64_t capacity_pages,
                                                                     uint64_t requested_pages, std::optional<HiCacheNodeId> node_id) const {
    const auto device_tier = tier == "L1";
    auto choice = HiCacheCapacityVictimChoice{
        .tier = tier,
        .reason = reason,
        .selected = node_id.has_value(),
        .node_id = node_id.value_or(0),
        .occupied_pages = device_tier ? snapshot_.occupied_device_pages : snapshot_.occupied_host_pages,
        .reserved_host_pages = device_tier ? 0 : snapshot_.reserved_host_pages,
        .capacity_pages = capacity_pages,
        .requested_pages = requested_pages,
        .excess_pages = device_tier ? device_excess_pages(capacity_pages) : host_excess_pages(capacity_pages),
    };
    if (!node_id) return choice;
    if (const auto record = indexed_record(*node_id)) {
        choice.page_count = record->page_count;
        choice.priority = record->priority;
        choice.last_access_order = record->last_access_order;
        choice.pages = record->pages;
    }
    return choice;
}

std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> HiCacheCapacityIndex::derive_tree_records(const HiCacheTokenRadixTree & tree) const {
    std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> records;
    for (const auto & node : tree.nodes()) {
        if (!node.active || node.id == 0) continue;
        records[node.id] = make_record(tree, node.id);
    }
    return records;
}

HiCacheCapacityMutation HiCacheCapacityIndex::sync_reservation(uint64_t reserved_host_pages, const std::string & reason) {
    reserved_host_pages_ = reserved_host_pages;
    const auto epoch = ++mutation_epoch_;
    update_snapshot();
    auto mutation = HiCacheCapacityMutation{
        .mutation_epoch = epoch,
        .reason = reason,
        .reserved_host_pages = reserved_host_pages_,
    };
    record_mutation(mutation);
    return mutation;
}

HiCacheCapacityMutation HiCacheCapacityIndex::sync_nodes(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes,
                                                         uint64_t reserved_host_pages, const std::string & reason) {
    reserved_host_pages_ = reserved_host_pages;
    const auto epoch = ++mutation_epoch_;
    auto mutation = HiCacheCapacityMutation{
        .mutation_epoch = epoch,
        .reason = reason,
        .reserved_host_pages = reserved_host_pages_,
    };

    for (const auto node_id : observation_closure(tree, seed_nodes)) {
        const auto old_it = records_.find(node_id);
        const auto old_device_leaf = old_it != records_.end() && old_it->second.device_evictable;
        const auto old_host_leaf = old_it != records_.end() && old_it->second.host_evictable;
        if (old_it != records_.end()) remove_record_contribution(old_it->second);

        auto record = make_record(tree, node_id);
        record.observed_epoch = epoch;
        if (!record.active) {
            records_.erase(node_id);
            if (old_device_leaf) mutation.device_leaf_left.push_back(node_id);
            if (old_host_leaf) mutation.host_leaf_left.push_back(node_id);
            continue;
        }

        const auto new_device_leaf = record.device_evictable;
        const auto new_host_leaf = record.host_evictable;
        add_record_contribution(record);
        records_[node_id] = std::move(record);
        mutation.observed_nodes.push_back(node_id);
        if (!old_device_leaf && new_device_leaf) mutation.device_leaf_entered.push_back(node_id);
        if (old_device_leaf && !new_device_leaf) mutation.device_leaf_left.push_back(node_id);
        if (!old_host_leaf && new_host_leaf) mutation.host_leaf_entered.push_back(node_id);
        if (old_host_leaf && !new_host_leaf) mutation.host_leaf_left.push_back(node_id);
    }

    update_snapshot();
    record_mutation(mutation);
    return mutation;
}

uint64_t HiCacheCapacityIndex::device_excess_pages(uint64_t capacity_pages) const { return excess(snapshot_.occupied_device_pages, capacity_pages); }

uint64_t HiCacheCapacityIndex::host_excess_pages(uint64_t capacity_pages) const {
    return excess(snapshot_.occupied_host_pages + snapshot_.reserved_host_pages, capacity_pages);
}

std::optional<HiCacheNodeId> HiCacheCapacityIndex::first_device_victim() const {
    if (snapshot_.evictable_device_leaves.empty()) return std::nullopt;
    return snapshot_.evictable_device_leaves.front();
}

std::optional<HiCacheNodeId> HiCacheCapacityIndex::first_host_victim() const {
    if (snapshot_.evictable_host_leaves.empty()) return std::nullopt;
    return snapshot_.evictable_host_leaves.front();
}

std::optional<HiCacheNodeId> HiCacheCapacityIndex::select_device_victim(uint64_t capacity_pages, uint64_t requested_pages, const std::string & reason) {
    const auto victim = first_device_victim();
    auto choice = make_victim_choice("L1", reason, capacity_pages, requested_pages, victim);
    choice.selection_epoch = ++victim_selection_epoch_;
    victim_choices_.push_back(std::move(choice));
    return victim;
}

std::optional<HiCacheNodeId> HiCacheCapacityIndex::select_host_victim(uint64_t capacity_pages, uint64_t requested_pages, const std::string & reason) {
    const auto victim = first_host_victim();
    auto choice = make_victim_choice("L2", reason, capacity_pages, requested_pages, victim);
    choice.selection_epoch = ++victim_selection_epoch_;
    victim_choices_.push_back(std::move(choice));
    return victim;
}

HiCacheCapacityAudit HiCacheCapacityIndex::audit(const HiCacheTokenRadixTree & tree, uint64_t expected_reserved_host_pages) const {
    HiCacheCapacityAudit audit{
        .indexed_device_pages = snapshot_.occupied_device_pages,
        .indexed_host_pages = snapshot_.occupied_host_pages,
        .indexed_storage_pages = snapshot_.readable_storage_pages,
        .indexed_reserved_host_pages = snapshot_.reserved_host_pages,
        .expected_reserved_host_pages = expected_reserved_host_pages,
    };

    const auto tree_records = derive_tree_records(tree);
    std::set<VictimKey> expected_device_leaves;
    std::set<VictimKey> expected_host_leaves;
    for (const auto & record : tree_records | std::views::values) {
        if (record.residency.device_present) audit.tree_device_pages += record.page_count;
        if (record.residency.host_present && record.residency.host_visible) audit.tree_host_pages += record.page_count;
        if (record.residency.storage_readable) audit.tree_storage_pages += record.page_count;
        if (record.device_evictable) expected_device_leaves.insert(victim_key(record));
        if (record.host_evictable) expected_host_leaves.insert(victim_key(record));
    }

    auto add_count_issue = [&](const std::string & issue, const std::string & tier, uint64_t indexed, uint64_t expected) {
        if (indexed == expected) return;
        audit.issues.push_back(HiCacheCapacityAuditIssue{
            .issue = issue,
            .tier = tier,
            .indexed_count = indexed,
            .tree_count = expected,
        });
    };
    add_count_issue("occupied_device_pages_mismatch", "L1", snapshot_.occupied_device_pages, audit.tree_device_pages);
    add_count_issue("occupied_host_pages_mismatch", "L2", snapshot_.occupied_host_pages, audit.tree_host_pages);
    add_count_issue("readable_storage_pages_mismatch", "L3", snapshot_.readable_storage_pages, audit.tree_storage_pages);
    add_count_issue("reserved_host_pages_mismatch", "L2", snapshot_.reserved_host_pages, expected_reserved_host_pages);

    auto expected_device_ids = std::vector<HiCacheNodeId>{};
    expected_device_ids.reserve(expected_device_leaves.size());
    std::ranges::transform(expected_device_leaves, std::back_inserter(expected_device_ids), &VictimKey::node_id);

    auto expected_host_ids = std::vector<HiCacheNodeId>{};
    expected_host_ids.reserve(expected_host_leaves.size());
    std::ranges::transform(expected_host_leaves, std::back_inserter(expected_host_ids), &VictimKey::node_id);
    if (snapshot_.evictable_device_leaves != expected_device_ids) {
        audit.issues.push_back(HiCacheCapacityAuditIssue{
            .issue = "device_leaf_set_mismatch",
            .tier = "L1",
            .indexed_count = static_cast<uint64_t>(snapshot_.evictable_device_leaves.size()),
            .tree_count = static_cast<uint64_t>(expected_device_ids.size()),
        });
    }
    if (snapshot_.evictable_host_leaves != expected_host_ids) {
        audit.issues.push_back(HiCacheCapacityAuditIssue{
            .issue = "host_leaf_set_mismatch",
            .tier = "L2",
            .indexed_count = static_cast<uint64_t>(snapshot_.evictable_host_leaves.size()),
            .tree_count = static_cast<uint64_t>(expected_host_ids.size()),
        });
    }

    for (const auto & [node_id, record] : records_) {
        const auto expected_it = tree_records.find(node_id);
        if (expected_it == tree_records.end()) {
            audit.issues.push_back(HiCacheCapacityAuditIssue{
                .issue = "stale_node_record",
                .node_id = node_id,
                .indexed_count = record.page_count,
                .tree_count = 0,
                .pages = record.pages,
            });
            continue;
        }
        const auto & expected = expected_it->second;
        const auto mismatch = record.page_count != expected.page_count || record.pages != expected.pages || record.priority != expected.priority
                              || record.last_access_order != expected.last_access_order || !same_residency(record.residency, expected.residency)
                              || !same_refs(record.refs, expected.refs) || record.device_evictable != expected.device_evictable
                              || record.host_evictable != expected.host_evictable;
        if (!mismatch) continue;
        audit.issues.push_back(HiCacheCapacityAuditIssue{
            .issue = "node_record_mismatch",
            .node_id = node_id,
            .indexed_count = record.page_count,
            .tree_count = expected.page_count,
            .pages = expected.pages,
        });
    }
    for (const auto & [node_id, expected] : tree_records) {
        if (records_.contains(node_id)) continue;
        audit.issues.push_back(HiCacheCapacityAuditIssue{
            .issue = "missing_node_record",
            .node_id = node_id,
            .indexed_count = 0,
            .tree_count = expected.page_count,
            .pages = expected.pages,
        });
    }
    return audit;
}

} // namespace TraceGraph
