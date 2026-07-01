/**
 * @file
 * @brief HiCache write-through/write-back backup 和 ACK 近似。
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

/**
 * @brief 在 target-control 或 finalize 边界释放 write-through backup 临时 ref。
 *
 * SGLang write-through-selective backup 在 CPU 写入 ACK 前会持有普通 lock ref。
 * 模型把 ACK 时序折叠到下一条 target control fact 开始处 drain；trace 末尾仍
 * pending 的 ACK 在 target finalize 边界收敛，避免 final state 保留临时 lock。
 */
void HiCacheState::drain_write_through_backup_refs(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                   ScopedState & scope, const std::string & reason) {
    if (scope.pending_write_through_backups.empty()) return;

    auto pending = std::exchange(scope.pending_write_through_backups, {});
    for (const auto & backup : pending) {
        const auto before = debug_state_digest();
        const auto ref = scope.refs.release_owner(scope.tree, backup.owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, reason);
        if constexpr (debug_records_enabled())
            record_transition(fact, summary, transitions, "complete_write_through_backup", "writeback", backup.pages, before);
    }
}

/**
 * @brief 将一个 device node 的 value 提交为 host/storage backup。
 *
 * storage_readable=true 表示 backup 同步进入 L3 backend 可读目录；write-back dirty
 * eviction 和 hit-count write-through backup 复用该入口，但 host allocation 必须先通过
 * target capacity cleanup。
 */
bool HiCacheState::commit_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                      HiCacheNodeId node_id, bool storage_readable) {
    const auto pages = scope.tree.node_pages(node_id);
    const auto * node = scope.tree.node(node_id);
    if (node == nullptr || pages.empty()) return false;
    const auto host_allocation_pages = !(node->residency.host_present && node->residency.host_visible) ? static_cast<uint64_t>(pages.size()) : uint64_t{ 0 };
    if (host_allocation_pages > 0) {
        const auto allocation = request_host_allocation(fact, summary, transitions, scope, host_allocation_pages, host_allocation_pages, false, "write_backup");
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "host_allocation",
                                       .policy_name = "write_backup",
                                       .decision = allocation.accepted ? "accept_host_backup_pages" : "skip_host_backup_capacity",
                                       .reason = allocation.accepted ? "target host pool can fit write_backup allocation after SGLang-style cleanup"
                                                                     : "target host pool still lacks space after SGLang-style cleanup",
                                       .accepted = allocation.accepted,
                                       .requested_pages = host_allocation_pages,
                                       .candidate_pages = allocation.accepted_pages,
                                       .capacity_pages = allocation.capacity_pages,
                                       .occupied_pages = allocation.occupied_pages,
                                       .reserved_pages = allocation.reserved_pages,
                                       .pages = pages,
                                   });
        if (!allocation.accepted) return false;
    }

    std::string storage_id;
    if (storage_readable) {
        storage_id = scope.clock.next_operation_id("storage");
        const auto storage_owner = scoped_request_key(fact) + ":storage:" + storage_id;
        const auto before_enqueue = debug_state_digest();
        scope.async_ops.upsert_storage(HiCacheStorageOperation{
            .header = make_operation_header(HiCacheOperationKind::Storage,
                                            storage_id,
                                            normalized_scope(fact),
                                            scoped_request_key(fact),
                                            storage_owner,
                                            node_id,
                                            std::vector<HiCacheNodeId>{ node_id },
                                            pages,
                                            fact.ts,
                                            0),
            .node_id = node_id,
        });
        const auto ref =
            scope.refs.acquire_host(scope.tree, storage_owner, "storage", scoped_request_key(fact), storage_id, std::vector<HiCacheNodeId>{ node_id });
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_acquire");
        if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "enqueue_storage_backup", "storage", pages, before_enqueue);
    }
    const auto before = debug_state_digest();
    scope.tree.mark_host_visible(node_id, storage_readable);
    scope.tree.clear_dirty(node_id);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "commit_host_backup");
    if (storage_readable) {
        scope.storage.mark_readable_pages(normalized_scope(fact), pages);
        scope.storage.mark_materialized_pages(pages, node_id);
    }
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, storage_readable ? "commit_host_storage_backup" : "commit_host_backup", "L2", pages, before);
    if (storage_readable && policy_.write_count_enabled()) { hold_write_through_backup_ref(fact, summary, transitions, scope, node_id, pages); }
    if (!storage_id.empty()) {
        const auto before_complete = debug_state_digest();
        scope.async_ops.set_storage_state(storage_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
        const auto ref = scope.refs.release_owner(scope.tree, scoped_request_key(fact) + ":storage:" + storage_id);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_release");
        if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "complete_storage_backup", "storage", pages, before_complete);
    }
    return true;
}

/**
 * @brief 为 write-through-selective backup 持有普通 lock ref，等待后续 ACK drain。
 *
 * 该 ref 不代表 request lifecycle，而是近似 SGLang `writing_check()` 之前 backup
 * node 仍被保护的窗口。
 */
void HiCacheState::hold_write_through_backup_ref(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                                 HiCacheNodeId node_id, const std::vector<std::string> & pages) {
    const auto chain = scope.tree.ancestor_node_ids(node_id);
    if (chain.empty()) return;

    const auto operation_id = scope.clock.next_operation_id("write_through_backup");
    const auto owner = scoped_request_key(fact) + ":write_through_backup:" + operation_id;
    const auto before = debug_state_digest();
    const auto ref = scope.refs.acquire_lock(scope.tree, owner, "write_through_backup", scoped_request_key(fact), operation_id, chain);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "write_through_backup_ref_acquire");
    scope.pending_write_through_backups.push_back(PendingWriteThroughBackup{
        .owner = owner,
        .pages = pages,
    });
    if constexpr (debug_records_enabled())
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = operation_id,
                                   .policy_area = "write_policy",
                                   .policy_name = policy_.write_policy(),
                                   .decision = "hold_write_through_backup_ref",
                                   .reason = "SGLang write_backup() protects non-write-back nodes until writing_check() observes the async CPU write ack",
                                   .accepted = true,
                                   .candidate_pages = static_cast<uint64_t>(pages.size()),
                                   .pages = pages,
                               });
    if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "enqueue_write_through_backup", "writeback", pages, before);
}

/**
 * @brief 按 hit-count write-through policy 检查 request path 上的 node 是否需要 backup。
 *
 * write-back 不走该路径；write-through-selective 在 node hit_count 达到 threshold 时
 * 立即提交 host/storage backup，并通过 hold_write_through_backup_ref 保留 ACK 前保护。
 */
void HiCacheState::apply_write_count_policy(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                            const std::vector<std::string> & pages) {
    if (!policy_.write_count_enabled()) {
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "write_policy",
                                       .policy_name = policy_.write_policy(),
                                       .decision = "skip_hit_count_backup",
                                       .reason = "target write policy does not use hit-count backup",
                                       .accepted = false,
                                       .candidate_pages = static_cast<uint64_t>(pages.size()),
                                       .pages = pages,
                                   });
        return;
    }
    const auto threshold = policy_.write_through_threshold();
    if (threshold == 0) {
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "write_policy",
                                       .policy_name = policy_.write_policy(),
                                       .decision = "skip_hit_count_backup",
                                       .reason = "resolved write-through threshold is zero",
                                       .accepted = false,
                                       .candidate_pages = static_cast<uint64_t>(pages.size()),
                                       .pages = pages,
                                   });
        return;
    }

    auto lookup = scope.tree.lookup_peek(pages);
    for (const auto node_id : lookup.topology_chain) {
        auto * node = scope.tree.mutable_node(node_id);
        if (node == nullptr || !node->residency.device_present) continue;
        const auto before = debug_state_digest();
        node->hit_count++;
        if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "increment_hit_count", "hit_count", node->pages, before);
        const auto should_backup = !has_host_backup(*node) && node->hit_count >= threshold;
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "write_policy",
                                       .policy_name = policy_.write_policy(),
                                       .decision = should_backup ? "commit_hit_count_backup" : "wait_for_hit_count_backup",
                                       .reason = has_host_backup(*node) ? "node already has host backup"
                                                 : should_backup        ? "node hit count reached write-through threshold"
                                                                        : "node hit count is below write-through threshold",
                                       .accepted = should_backup,
                                       .candidate_pages = static_cast<uint64_t>(node->pages.size()),
                                       .hit_count = node->hit_count,
                                       .threshold_pages = threshold,
                                       .pages = node->pages,
                                   });
        if (should_backup) (void)commit_host_backup(fact, summary, transitions, scope, node_id, true);
    }
}


} // namespace markov::trace_graph::modules::hicache::model
