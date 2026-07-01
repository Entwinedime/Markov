/**
 * @file
 * @brief HiCache node ref owner ledger 实现。
 */
#include "markov/trace_graph/modules/hicache/runtime/ref_ledger.hpp"

#include <algorithm>
#include <ranges>
#include <set>
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

std::vector<std::string> HiCacheRefLedger::flatten_pages(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & nodes) {
    std::vector<std::string> pages;
    std::ranges::for_each(nodes, [&](auto node_id) {
        auto node_pages = tree.node_pages(node_id);
        pages.insert(pages.end(), node_pages.begin(), node_pages.end());
    });
    return pages;
}

/**
 * @brief 取得或创建 ref owner 记录。
 *
 * owner 维度保留 request/operation 归属，便于一次释放整条链上的 lock/host ref；
 * radix node 上的 ref counter 才是 capacity/victim 判断会读取的 canonical copy。
 */
HiCacheRefOwnerRecord & HiCacheRefLedger::ensure_owner(const std::string & owner_id, const std::string & owner_kind, const std::string & request_key,
                                                       const std::string & operation_id) {
    auto & record = owners_[owner_id];
    if (record.owner_id.empty()) record.owner_id = owner_id;
    if (!owner_kind.empty()) record.owner_kind = owner_kind;
    if (!request_key.empty()) record.request_key = request_key;
    if (!operation_id.empty()) record.operation_id = operation_id;
    return record;
}

void HiCacheRefLedger::record_mutation(HiCacheRefMutation mutation) {
    if (!mutation.changed) return;
    if (mutation.mutation_epoch == 0) mutation.mutation_epoch = ++epoch_;
#ifdef DEBUG
    mutation_trace_.push_back(std::move(mutation));
#else
    (void)mutation;
#endif
}

HiCacheRefMutation HiCacheRefLedger::release_owner(HiCacheTokenRadixTree & tree, const std::string & owner_id) {
    /**
     * @brief release_owner 是唯一按 owner 批量回收 ref 的入口。
     *
     * 它同时更新 radix node counter 和 ledger owner 状态，避免两份 ref copy 漂移。
     */
    HiCacheRefMutation mutation{
        .action = "release",
        .owner_id = owner_id,
    };
    const auto it = owners_.find(owner_id);
    if (it == owners_.end() || !it->second.active) return mutation;

    auto & record = it->second;
    mutation.owner_kind = record.owner_kind;
    mutation.request_key = record.request_key;
    mutation.operation_id = record.operation_id;
    mutation.lock_nodes = record.lock_nodes;
    mutation.host_nodes = record.host_nodes;
    mutation.lock_pages = flatten_pages(tree, record.lock_nodes);
    mutation.host_pages = flatten_pages(tree, record.host_nodes);
    mutation.lock_ref_delta = -static_cast<int64_t>(record.lock_nodes.size());
    mutation.host_ref_delta = -static_cast<int64_t>(record.host_nodes.size());
    mutation.changed = !record.lock_nodes.empty() || !record.host_nodes.empty();

    tree.release_refs_by_owner(owner_id);
    record.lock_nodes.clear();
    record.host_nodes.clear();
    mutation.mutation_epoch = ++epoch_;
    record.release_epoch = mutation.mutation_epoch;
    record.active = false;
    record_mutation(mutation);
    return mutation;
}

HiCacheRefMutation HiCacheRefLedger::acquire_lock(HiCacheTokenRadixTree & tree, const std::string & owner_id, const std::string & owner_kind,
                                                  const std::string & request_key, const std::string & operation_id, const std::vector<HiCacheNodeId> & nodes) {
    /**
     * @brief lock ref 保护 device/host eviction 路径。
     *
     * 常见用途是 request 执行或 backup ACK 尚未完成的窗口。
     */
    HiCacheRefMutation mutation{
        .action = "acquire_lock",
        .owner_id = owner_id,
        .owner_kind = owner_kind,
        .request_key = request_key,
        .operation_id = operation_id,
    };
    if (owner_id.empty() || nodes.empty()) return mutation;

    auto & record = ensure_owner(owner_id, owner_kind, request_key, operation_id);
    tree.add_lock_ref(nodes, owner_id);
    record.lock_nodes.insert(record.lock_nodes.end(), nodes.begin(), nodes.end());
    mutation.mutation_epoch = ++epoch_;
    record.acquire_epoch = mutation.mutation_epoch;
    record.active = true;
    mutation.lock_nodes = nodes;
    mutation.lock_pages = flatten_pages(tree, nodes);
    mutation.lock_ref_delta = static_cast<int64_t>(nodes.size());
    mutation.changed = !mutation.lock_pages.empty();
    record_mutation(mutation);
    return mutation;
}

HiCacheRefMutation HiCacheRefLedger::acquire_host(HiCacheTokenRadixTree & tree, const std::string & owner_id, const std::string & owner_kind,
                                                  const std::string & request_key, const std::string & operation_id, const std::vector<HiCacheNodeId> & nodes) {
    /**
     * @brief host ref 保护 L2 value。
     *
     * 它覆盖 prefetch materialize 前后的 reservation/use 窗口；capacity cleanup 会跳过仍有 host ref 的 leaf。
     */
    HiCacheRefMutation mutation{
        .action = "acquire_host",
        .owner_id = owner_id,
        .owner_kind = owner_kind,
        .request_key = request_key,
        .operation_id = operation_id,
    };
    if (owner_id.empty() || nodes.empty()) return mutation;

    auto & record = ensure_owner(owner_id, owner_kind, request_key, operation_id);
    tree.add_host_ref(nodes, owner_id);
    record.host_nodes.insert(record.host_nodes.end(), nodes.begin(), nodes.end());
    mutation.mutation_epoch = ++epoch_;
    record.acquire_epoch = mutation.mutation_epoch;
    record.active = true;
    mutation.host_nodes = nodes;
    mutation.host_pages = flatten_pages(tree, nodes);
    mutation.host_ref_delta = static_cast<int64_t>(nodes.size());
    mutation.changed = !mutation.host_pages.empty();
    record_mutation(mutation);
    return mutation;
}

void HiCacheRefLedger::sync_tree_ref_copies(const HiCacheTokenRadixTree & tree, const std::string & reason) {
    /**
     * @brief radix split 会把 child ref 复制到新 prefix node。
     *
     * ledger 需要追平这些复制出来的 owner 计数，否则后续 release_owner 只能释放原 node，
     * 留下不可释放的 ref。
     */
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
            record.acquire_epoch = ++epoch_;
            record_mutation(HiCacheRefMutation{
                .mutation_epoch = record.acquire_epoch,
                .action = "sync_lock_ref_copy",
                .owner_id = owner_id,
                .owner_kind = record.owner_kind,
                .request_key = record.request_key,
                .operation_id = record.operation_id,
                .reason = reason,
                .lock_nodes = copied_nodes,
                .lock_pages = flatten_pages(tree, copied_nodes),
                .lock_ref_delta = static_cast<int64_t>(missing),
                .changed = true,
            });
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
            record.acquire_epoch = ++epoch_;
            record_mutation(HiCacheRefMutation{
                .mutation_epoch = record.acquire_epoch,
                .action = "sync_host_ref_copy",
                .owner_id = owner_id,
                .owner_kind = record.owner_kind,
                .request_key = record.request_key,
                .operation_id = record.operation_id,
                .reason = reason,
                .host_nodes = copied_nodes,
                .host_pages = flatten_pages(tree, copied_nodes),
                .host_ref_delta = static_cast<int64_t>(missing),
                .changed = true,
            });
        }
    }
}

const HiCacheRefOwnerRecord * HiCacheRefLedger::owner(const std::string & owner_id) const {
    const auto it = owners_.find(owner_id);
    return it == owners_.end() ? nullptr : &it->second;
}

#ifdef DEBUG
HiCacheRefAudit HiCacheRefLedger::audit(const HiCacheTokenRadixTree & tree) const {
    /**
     * @brief audit 比较 owner ledger 与 radix node ref maps。
     *
     * 它用于调试和 summary，不是 ref 状态的第三份业务真相源。
     */
    HiCacheRefAudit audit;
    std::map<NodeOwnerKey, uint64_t> ledger_lock_refs;
    std::map<NodeOwnerKey, uint64_t> ledger_host_refs;
    std::map<NodeOwnerKey, uint64_t> tree_lock_refs;
    std::map<NodeOwnerKey, uint64_t> tree_host_refs;

    for (const auto & record : owners_ | std::views::values) {
        if (!record.active) continue;
        audit.active_owner_count++;
        std::ranges::for_each(record.lock_nodes, [&](auto node_id) {
            ledger_lock_refs[{ node_id, record.owner_id }]++;
            audit.ledger_lock_ref_count++;
        });
        std::ranges::for_each(record.host_nodes, [&](auto node_id) {
            ledger_host_refs[{ node_id, record.owner_id }]++;
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
#endif

uint64_t HiCacheRefLedger::active_owner_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(owners_, [](const auto & item) { return item.second.active; }));
}

} // namespace markov::trace_graph::modules::hicache::runtime
