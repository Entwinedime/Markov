/**
 * @file
 * @brief HiCache node-reference owner ledger implementation.
 */
#include "markov/trace_graph/modules/hicache/runtime/ref_ledger.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace ref_ledger_detail {

#ifdef DEBUG
using NodeOwnerKey = std::pair<HiCacheNodeId, std::string>;
#endif

uint64_t count_node(const std::vector<HiCacheNodeId> & nodes, HiCacheNodeId node_id) { return static_cast<uint64_t>(std::ranges::count(nodes, node_id)); }

#ifdef DEBUG
uint64_t map_value(const std::map<NodeOwnerKey, uint64_t> & values, const NodeOwnerKey & key) {
    const auto it = values.find(key);
    return it == values.end() ? 0 : it->second;
}
#endif

std::vector<HiCacheNodeId> repeated_nodes(HiCacheNodeId node_id, uint64_t count) { return std::vector<HiCacheNodeId>(static_cast<size_t>(count), node_id); }

} // namespace ref_ledger_detail

using ref_ledger_detail::count_node;
#ifdef DEBUG
using ref_ledger_detail::map_value;
using ref_ledger_detail::NodeOwnerKey;
#endif
using ref_ledger_detail::repeated_nodes;

#ifdef DEBUG
std::vector<std::string> HiCacheRefLedger::flatten_pages(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & nodes) {
    std::vector<std::string> pages;
    std::ranges::for_each(nodes, [&](auto node_id) {
        const auto & node_pages = tree.node_pages(node_id);
        pages.insert(pages.end(), node_pages.begin(), node_pages.end());
    });
    return pages;
}
#endif

/**
 * @brief Returns or creates the reverse-index record for one reference owner.
 *
 * The owner dimension permits an atomic lifecycle release across lock and host chains.
 * Capacity and victim policy continue to read the canonical counters on radix nodes.
 */
HiCacheRefOwnerRecord & HiCacheRefLedger::ensure_owner(const std::string & owner_id, const OwnerMetadata & metadata) {
    auto & record = owners_[owner_id];
#ifdef DEBUG
    auto assign_identity = [&](std::string & current, const std::string & incoming, std::string_view field) {
        if (incoming.empty()) return;
        if (!current.empty() && current != incoming) throw std::logic_error("Conflicting HiCache reference owner " + std::string(field) + ": " + owner_id);
        current = incoming;
    };
    assign_identity(record.owner_kind, metadata.owner_kind, "kind");
    assign_identity(record.request_key, metadata.request_key, "request key");
    assign_identity(record.operation_id, metadata.operation_id, "operation ID");
#else
    (void)metadata;
#endif
    return record;
}

HiCacheRefChange HiCacheRefLedger::release_owner(HiCacheTokenRadixTree & tree, const std::string & owner_id) {
    // This is the only owner-wide release path. It updates radix counters and the reverse
    // index together so copied references cannot outlive their lifecycle owner.
    auto change = HiCacheRefChange{};
    const auto it = owners_.find(owner_id);
    if (it == owners_.end() || !it->second.active) return change;

    auto & record = it->second;
#ifdef DEBUG
    auto mutation = HiCacheRefMutation{
        .action = "release",
        .owner_id = owner_id,
        .owner_kind = record.owner_kind,
        .request_key = record.request_key,
        .operation_id = record.operation_id,
        .lock_nodes = record.lock_nodes,
        .host_nodes = record.host_nodes,
        .lock_pages = flatten_pages(tree, record.lock_nodes),
        .host_pages = flatten_pages(tree, record.host_nodes),
        .lock_ref_delta = -static_cast<int64_t>(record.lock_nodes.size()),
        .host_ref_delta = -static_cast<int64_t>(record.host_nodes.size()),
        .changed = !record.lock_nodes.empty() || !record.host_nodes.empty(),
    };
#endif
    change.affected_nodes.reserve(record.lock_nodes.size() + record.host_nodes.size());
    change.affected_nodes.insert(change.affected_nodes.end(), record.lock_nodes.begin(), record.lock_nodes.end());
    change.affected_nodes.insert(change.affected_nodes.end(), record.host_nodes.begin(), record.host_nodes.end());

    tree.release_refs_by_owner(owner_id);
    record.lock_nodes.clear();
    record.host_nodes.clear();
#ifdef DEBUG
    mutation.mutation_epoch = core::checked_increment_u64(epoch_, "HiCache reference mutation epoch exceeds uint64 range");
    record.release_epoch = mutation.mutation_epoch;
#endif
    record.active = false;
#ifdef DEBUG
    if (mutation.changed) mutation_trace_.push_back(std::move(mutation));
#endif
    return change;
}

HiCacheRefChange HiCacheRefLedger::acquire_lock(HiCacheTokenRadixTree & tree, const std::string & owner_id, const std::string & owner_kind,
                                                const std::string & request_key, const std::string & operation_id, const std::vector<HiCacheNodeId> & nodes) {
    // Lock references protect device and host eviction while a request executes or a
    // backup acknowledgement is outstanding.
    if (owner_id.empty() || nodes.empty()) return {};

    auto & record = ensure_owner(owner_id,
                                 OwnerMetadata{
                                     .owner_kind = owner_kind,
                                     .request_key = request_key,
                                     .operation_id = operation_id,
                                 });
    tree.add_lock_ref(nodes, owner_id);
    record.lock_nodes.insert(record.lock_nodes.end(), nodes.begin(), nodes.end());
#ifdef DEBUG
    auto mutation = HiCacheRefMutation{
        .mutation_epoch = core::checked_increment_u64(epoch_, "HiCache reference mutation epoch exceeds uint64 range"),
        .action = "acquire_lock",
        .owner_id = owner_id,
        .owner_kind = owner_kind,
        .request_key = request_key,
        .operation_id = operation_id,
        .lock_nodes = nodes,
        .lock_pages = flatten_pages(tree, nodes),
        .lock_ref_delta = static_cast<int64_t>(nodes.size()),
    };
    record.acquire_epoch = mutation.mutation_epoch;
#endif
    record.active = true;
#ifdef DEBUG
    mutation.changed = !mutation.lock_pages.empty();
    if (mutation.changed) mutation_trace_.push_back(std::move(mutation));
#endif
    return HiCacheRefChange{ .affected_nodes = nodes };
}

HiCacheRefChange HiCacheRefLedger::acquire_host(HiCacheTokenRadixTree & tree, const std::string & owner_id, const std::string & owner_kind,
                                                const std::string & request_key, const std::string & operation_id, const std::vector<HiCacheNodeId> & nodes) {
    // Host references protect L2 values throughout prefetch reservation and consumption;
    // capacity cleanup must skip leaves that still carry one.
    if (owner_id.empty() || nodes.empty()) return {};

    auto & record = ensure_owner(owner_id,
                                 OwnerMetadata{
                                     .owner_kind = owner_kind,
                                     .request_key = request_key,
                                     .operation_id = operation_id,
                                 });
    tree.add_host_ref(nodes, owner_id);
    record.host_nodes.insert(record.host_nodes.end(), nodes.begin(), nodes.end());
#ifdef DEBUG
    auto mutation = HiCacheRefMutation{
        .mutation_epoch = core::checked_increment_u64(epoch_, "HiCache reference mutation epoch exceeds uint64 range"),
        .action = "acquire_host",
        .owner_id = owner_id,
        .owner_kind = owner_kind,
        .request_key = request_key,
        .operation_id = operation_id,
        .host_nodes = nodes,
        .host_pages = flatten_pages(tree, nodes),
        .host_ref_delta = static_cast<int64_t>(nodes.size()),
    };
    record.acquire_epoch = mutation.mutation_epoch;
#endif
    record.active = true;
#ifdef DEBUG
    mutation.changed = !mutation.host_pages.empty();
    if (mutation.changed) mutation_trace_.push_back(std::move(mutation));
#endif
    return HiCacheRefChange{ .affected_nodes = nodes };
}

void HiCacheRefLedger::sync_tree_ref_copies(const HiCacheTokenRadixTree & tree, std::string_view reason) {
    // Radix splits copy child references to a new prefix node. Mirror those copies into
    // owner chains or release_owner would leave the new counters behind.
    for (const auto & node : tree.nodes()) {
        if (!node.active || node.id == 0) continue;
        for (const auto & [owner_id, tree_count] : node.refs.lock_refs_by_owner) {
            auto owner_it = owners_.find(owner_id);
            if (owner_it == owners_.end() || !owner_it->second.active) continue;
            auto & record = owner_it->second;
            const auto ledger_count = count_node(record.lock_nodes, node.id);
            if (tree_count <= ledger_count) continue;
            const auto missing = tree_count - ledger_count;
            auto copied_nodes = repeated_nodes(node.id, missing);
            record.lock_nodes.insert(record.lock_nodes.end(), copied_nodes.begin(), copied_nodes.end());
#ifdef DEBUG
            record.acquire_epoch = core::checked_increment_u64(epoch_, "HiCache reference mutation epoch exceeds uint64 range");
            mutation_trace_.push_back(HiCacheRefMutation{
                .mutation_epoch = record.acquire_epoch,
                .action = "sync_lock_ref_copy",
                .owner_id = owner_id,
                .owner_kind = record.owner_kind,
                .request_key = record.request_key,
                .operation_id = record.operation_id,
                .reason = std::string(reason),
                .lock_nodes = copied_nodes,
                .lock_pages = flatten_pages(tree, copied_nodes),
                .lock_ref_delta = static_cast<int64_t>(missing),
                .changed = true,
            });
#endif
        }
        for (const auto & [owner_id, tree_count] : node.refs.host_refs_by_owner) {
            auto owner_it = owners_.find(owner_id);
            if (owner_it == owners_.end() || !owner_it->second.active) continue;
            auto & record = owner_it->second;
            const auto ledger_count = count_node(record.host_nodes, node.id);
            if (tree_count <= ledger_count) continue;
            const auto missing = tree_count - ledger_count;
            auto copied_nodes = repeated_nodes(node.id, missing);
            record.host_nodes.insert(record.host_nodes.end(), copied_nodes.begin(), copied_nodes.end());
#ifdef DEBUG
            record.acquire_epoch = core::checked_increment_u64(epoch_, "HiCache reference mutation epoch exceeds uint64 range");
            mutation_trace_.push_back(HiCacheRefMutation{
                .mutation_epoch = record.acquire_epoch,
                .action = "sync_host_ref_copy",
                .owner_id = owner_id,
                .owner_kind = record.owner_kind,
                .request_key = record.request_key,
                .operation_id = record.operation_id,
                .reason = std::string(reason),
                .host_nodes = copied_nodes,
                .host_pages = flatten_pages(tree, copied_nodes),
                .host_ref_delta = static_cast<int64_t>(missing),
                .changed = true,
            });
#endif
        }
    }
#ifndef DEBUG
    (void)reason;
#endif
}

#ifdef DEBUG
HiCacheRefAudit HiCacheRefLedger::audit(const HiCacheTokenRadixTree & tree) const {
    // This independent comparison supports Debug summaries only; it is never a third
    // reference-state source and does not participate in policy decisions.
    HiCacheRefAudit audit;
    std::map<NodeOwnerKey, uint64_t> ledger_lock_refs;
    std::map<NodeOwnerKey, uint64_t> ledger_host_refs;
    std::map<NodeOwnerKey, uint64_t> tree_lock_refs;
    std::map<NodeOwnerKey, uint64_t> tree_host_refs;

    for (const auto & [owner_id, record] : owners_) {
        if (!record.active) continue;
        audit.active_owner_count++;
        std::ranges::for_each(record.lock_nodes, [&](auto node_id) {
            ledger_lock_refs[{ node_id, owner_id }]++;
            audit.ledger_lock_ref_count++;
        });
        std::ranges::for_each(record.host_nodes, [&](auto node_id) {
            ledger_host_refs[{ node_id, owner_id }]++;
            audit.ledger_host_ref_count++;
        });
    }

    for (const auto & node : tree.nodes()) {
        if (!node.active || node.id == 0) continue;
        uint64_t lock_owner_sum = 0;
        uint64_t host_owner_sum = 0;
        for (const auto count : node.refs.lock_refs_by_owner | std::views::values) { lock_owner_sum += count; }
        for (const auto count : node.refs.host_refs_by_owner | std::views::values) { host_owner_sum += count; }
        audit.tree_lock_ref_count += node.refs.lock_ref_total;
        audit.tree_host_ref_count += node.refs.host_ref_total;
        if (lock_owner_sum != node.refs.lock_ref_total) {
            audit.issues.push_back(HiCacheRefAuditIssue{
                .issue = "tree_lock_total_mismatch",
                .ref_kind = "lock",
                .node_id = node.id,
                .ledger_count = lock_owner_sum,
                .tree_count = node.refs.lock_ref_total,
                .pages = node.pages,
            });
        }
        if (host_owner_sum != node.refs.host_ref_total) {
            audit.issues.push_back(HiCacheRefAuditIssue{
                .issue = "tree_host_total_mismatch",
                .ref_kind = "host",
                .node_id = node.id,
                .ledger_count = host_owner_sum,
                .tree_count = node.refs.host_ref_total,
                .pages = node.pages,
            });
        }
        for (const auto & [owner_id, count] : node.refs.lock_refs_by_owner) { tree_lock_refs[{ node.id, owner_id }] = count; }
        for (const auto & [owner_id, count] : node.refs.host_refs_by_owner) { tree_host_refs[{ node.id, owner_id }] = count; }
    }

    auto compare_refs = [&](const std::string & kind, const auto & ledger_refs, const auto & tree_refs) {
        std::set<NodeOwnerKey> keys;
        std::ranges::copy(ledger_refs | std::views::keys, std::inserter(keys, keys.end()));
        std::ranges::copy(tree_refs | std::views::keys, std::inserter(keys, keys.end()));
        for (const auto & key : keys) {
            const auto ledger_count = map_value(ledger_refs, key);
            const auto tree_count = map_value(tree_refs, key);
            if (ledger_count == tree_count) continue;
            const auto * node = tree.node(key.first);
            audit.issues.push_back(HiCacheRefAuditIssue{
                .issue = kind + "_owner_count_mismatch",
                .ref_kind = kind,
                .owner_id = key.second,
                .node_id = key.first,
                .ledger_count = ledger_count,
                .tree_count = tree_count,
                .pages = node == nullptr ? std::vector<std::string>{} : node->pages,
            });
        }
    };

    compare_refs("lock", ledger_lock_refs, tree_lock_refs);
    compare_refs("host", ledger_host_refs, tree_host_refs);
    return audit;
}
uint64_t HiCacheRefLedger::active_owner_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(owners_, [](const auto & item) { return item.second.active; }));
}
#endif

} // namespace markov::trace_graph::modules::hicache::runtime
