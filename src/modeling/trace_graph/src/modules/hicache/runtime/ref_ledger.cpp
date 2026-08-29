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


uint64_t count_node(const std::vector<HiCacheNodeId> & nodes, HiCacheNodeId node_id) { return static_cast<uint64_t>(std::ranges::count(nodes, node_id)); }


std::vector<HiCacheNodeId> repeated_nodes(HiCacheNodeId node_id, uint64_t count) { return std::vector<HiCacheNodeId>(static_cast<size_t>(count), node_id); }

} // namespace ref_ledger_detail

using ref_ledger_detail::count_node;
using ref_ledger_detail::repeated_nodes;


/**
 * @brief Returns or creates the reverse-index record for one reference owner.
 *
 * The owner dimension permits an atomic lifecycle release across lock and host chains.
 * Capacity and victim policy continue to read the canonical counters on radix nodes.
 */
HiCacheRefOwnerRecord & HiCacheRefLedger::ensure_owner(const std::string & owner_id, const OwnerMetadata & metadata) {
    auto & record = owners_[owner_id];
    (void)metadata;
    return record;
}

HiCacheRefChange HiCacheRefLedger::release_owner(HiCacheTokenRadixTree & tree, const std::string & owner_id) {
    // This is the only owner-wide release path. It updates radix counters and the reverse
    // index together so copied references cannot outlive their lifecycle owner.
    auto change = HiCacheRefChange{};
    const auto it = owners_.find(owner_id);
    if (it == owners_.end() || !it->second.active) return change;

    auto & record = it->second;
    change.affected_nodes.reserve(record.lock_nodes.size() + record.host_nodes.size());
    change.affected_nodes.insert(change.affected_nodes.end(), record.lock_nodes.begin(), record.lock_nodes.end());
    change.affected_nodes.insert(change.affected_nodes.end(), record.host_nodes.begin(), record.host_nodes.end());

    tree.release_refs_by_owner(owner_id);
    record.lock_nodes.clear();
    record.host_nodes.clear();
    record.active = false;
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
    record.active = true;
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
    record.active = true;
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
        }
    }
#ifndef DEBUG
    (void)reason;
#endif
}


} // namespace markov::trace_graph::modules::hicache::runtime
