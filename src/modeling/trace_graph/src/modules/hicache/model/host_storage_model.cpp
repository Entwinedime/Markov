/**
 * @file
 * @brief HiCache host capacity、storage backup 和 device eviction 建模。
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
 * @brief 驱逐单个 device node，并在 write-back dirty 时先同步 host/storage backup。
 *
 * device eviction 同时影响 radix residency、allocator release queue、capacity index 和
 * async writeback trace。dirty write-back ACK 当前折叠为同步 completion，但仍保留
 * operation/ref lifecycle 便于 transition validator 观察。
 */
uint64_t HiCacheState::evict_device_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         ScopedState & scope, HiCacheNodeId node_id) {
    auto * node = scope.tree.mutable_node(node_id);
    if (node == nullptr || !node->residency.device_present) return 0;

    const auto pages = node->pages;
    const auto released_pages = static_cast<uint64_t>(pages.size());
    const bool needs_writeback = policy_.write_back_enabled() && node->residency.device_dirty;
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "write_policy",
                               .policy_name = policy_.write_policy(),
                               .decision = needs_writeback ? "enqueue_dirty_eviction_writeback" : "evict_without_writeback",
                               .reason = needs_writeback                ? "write_back dirty device node must refresh host backup before eviction"
                                         : node->residency.device_dirty ? "target write policy does not require dirty eviction writeback"
                                                                        : "device node is clean at eviction boundary",
                               .accepted = needs_writeback,
                               .candidate_pages = static_cast<uint64_t>(pages.size()),
                               .pages = pages,
                           });
    if (needs_writeback) {
        summary.dirty_eviction_events++;
        const auto writeback_id = scope.clock.next_operation_id("writeback");
        const auto writeback_owner = scoped_request_key(fact) + ":writeback:" + writeback_id;
        const auto before_enqueue = digest();
        scope.async_ops.upsert_writeback(HiCacheWritebackOperation{
            .header = make_operation_header(HiCacheOperationKind::Writeback,
                                            writeback_id,
                                            normalized_scope(fact),
                                            scoped_request_key(fact),
                                            writeback_owner,
                                            node_id,
                                            std::vector<HiCacheNodeId>{ node_id },
                                            pages,
                                            fact.ts,
                                            0),
            .node_id = node_id,
        });
        auto ref =
            scope.refs.acquire_lock(scope.tree, writeback_owner, "writeback", scoped_request_key(fact), writeback_id, std::vector<HiCacheNodeId>{ node_id });
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "writeback_ref_acquire");
        record_transition(fact, summary, transitions, "enqueue_writeback", "writeback", pages, before_enqueue);
        const auto committed = commit_host_backup(fact, summary, transitions, scope, node_id, true);
        const auto before_complete = digest();
        scope.async_ops.set_writeback_state(writeback_id,
                                            committed ? HiCacheOperationState::Committed : HiCacheOperationState::Cancelled,
                                            committed ? "sync_commit" : "host_backup_capacity_rejected",
                                            fact.ts);
        ref = scope.refs.release_owner(scope.tree, writeback_owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "writeback_ref_release");
        record_transition(fact, summary, transitions, committed ? "complete_writeback" : "cancel_writeback", "writeback", pages, before_complete);
        if (!committed) return 0;
    }

    const auto before = digest();
    if (has_host_backup(*node)) scope.tree.demote_device_to_host(node_id, false);
    else scope.tree.remove_device_regular(node_id);
    const auto allocator_released_pages = scope.device_allocator.release(released_pages);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "evict_device_node");
    record_transition(fact, summary, transitions, "evict_l1_node", "L1", pages, before);
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "device_allocator",
                               .policy_name = "device_eviction_free",
                               .decision = "release_device_pages_to_allocator",
                               .reason = "sglang device eviction frees the victim node value back to token_to_kv_pool_allocator",
                               .accepted = allocator_released_pages > 0,
                               .candidate_pages = released_pages,
                               .capacity_pages = scope.device_allocator.capacity_pages,
                               .allocator_free_pages = scope.device_allocator.free_pages,
                               .allocator_release_pages = scope.device_allocator.release_pages,
                               .allocator_available_pages = scope.device_allocator.available_pages(),
                               .allocator_released_pages = allocator_released_pages,
                               .pages = pages,
                           });
    return released_pages;
}

/**
 * @brief 尝试驱逐一个 host leaf。
 *
 * SGLang host cleanup 会跳过 host_ref_counter 仍为正的 leaf；成功驱逐时删除的是
 * host leaf/subtree，而不是只清空 node.host_value。
 */
uint64_t HiCacheState::evict_host_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                       ScopedState & scope, HiCacheNodeId node_id) {
    const auto * node = scope.tree.node(node_id);
    const auto pages = node == nullptr ? std::vector<std::string>{} : node->pages;
    if (node != nullptr && node->refs.host_ref_total > 0) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "host_cleanup",
                                   .policy_name = "evict_host",
                                   .decision = "skip_host_ref_protected_leaf",
                                   .reason = "SGLang evict_host skips a popped host leaf while host_ref_counter is positive",
                                   .accepted = false,
                                   .candidate_pages = static_cast<uint64_t>(pages.size()),
                                   .pages = pages,
                               });
        sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "evict_host_ref_protected");
        return 0;
    }

    const auto before = digest();
    const auto result = scope.tree.evict_host_leaf(node_id);
    sync_capacity(scope,
                  normalized_scope(fact),
                  result.affected_nodes.empty() ? std::vector<HiCacheNodeId>{ node_id } : result.affected_nodes,
                  result.evicted ? "evict_host_leaf" : "evict_host_leaf_skipped");
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "host_cleanup",
                               .policy_name = "evict_host",
                               .decision = result.evicted ? "evict_host_leaf" : "skip_host_leaf",
                               .reason = result.reason,
                               .accepted = result.evicted,
                               .candidate_pages = static_cast<uint64_t>(result.pages.size()),
                               .pages = result.pages,
                           });
    if (!result.evicted) return 0;
    record_transition(fact, summary, transitions, "evict_host_node", "L2", pages, before);
    return static_cast<uint64_t>(pages.size());
}

/**
 * @brief 按 SGLang device allocator available_size gate 执行 device cleanup。
 *
 * 当前模型不是按 final occupancy 超额直接清理，而是模拟 allocator 在申请前看到的
 * available_size：不足时把完整 request budget 交给 tree eviction。
 */
void HiCacheState::enforce_device_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                           ScopedState & scope, uint64_t requested_pages) {
    ensure_device_allocator(scope);
    const auto capacity = scope.device_allocator.capacity_pages;
    if (capacity == 0 || requested_pages == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, "device_allocator_budget");
    if (!scope.device_allocator.should_evict(requested_pages)) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "device_allocator",
                                   .policy_name = "available_size_gate",
                                   .decision = "skip_device_eviction",
                                   .reason = "sglang standard allocator evicts only when available_size is below request budget",
                                   .accepted = false,
                                   .requested_pages = requested_pages,
                                   .capacity_pages = capacity,
                                   .allocator_free_pages = scope.device_allocator.free_pages,
                                   .allocator_release_pages = scope.device_allocator.release_pages,
                                   .allocator_available_pages = scope.device_allocator.available_pages(),
                               });
        return;
    }
    auto target = requested_pages;
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "device_allocator",
                               .policy_name = "available_size_gate",
                               .decision = "evict_for_device_allocation",
                               .reason = "sglang standard allocator passes the full request budget to tree_cache.evict once available_size is insufficient",
                               .accepted = true,
                               .requested_pages = requested_pages,
                               .capacity_pages = capacity,
                               .allocator_free_pages = scope.device_allocator.free_pages,
                               .allocator_release_pages = scope.device_allocator.release_pages,
                               .allocator_available_pages = scope.device_allocator.available_pages(),
                           });
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {}, "device_allocator_loop");
        const auto victim = scope.capacity.select_device_victim(capacity, requested_pages, "device_allocator_loop");
        if (!victim) break;
        const auto released_pages = evict_device_node(fact, summary, transitions, scope, *victim);
        if (released_pages == 0 || released_pages >= target) break;
        target -= released_pages;
    }
}

/**
 * @brief 在 host allocation 或 capacity audit 边界执行 host cleanup。
 *
 * requested_pages>0 时使用申请预算驱动 cleanup；requested_pages==0 时只清理已经超过
 * capacity 的部分。reserved_host_pages 也计入压力，避免 active/pending prefetch 被忽略。
 */
void HiCacheState::enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         ScopedState & scope, uint64_t requested_pages) {
    const auto capacity = policy_.l2_capacity_pages();
    if (capacity == 0) return;
    const auto budget_reason = requested_pages > 0 ? "host_allocation_request_budget" : "host_capacity_budget";
    const auto loop_reason = requested_pages > 0 ? "host_allocation_request_loop" : "host_capacity_loop";
    sync_capacity(scope, normalized_scope(fact), {}, budget_reason);
    const auto snapshot = scope.capacity.snapshot();
    auto target = allocation_cleanup_target(snapshot.occupied_host_pages, snapshot.reserved_host_pages, capacity, requested_pages);
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {}, loop_reason);
        const auto victim = scope.capacity.select_host_victim(capacity, requested_pages, loop_reason);
        if (!victim) break;
        const auto victim_pages = evict_host_node(fact, summary, transitions, scope, *victim);
        if (victim_pages == 0) break;
        if (victim_pages >= target) break;
        target -= victim_pages;
    }
}

/**
 * @brief 申请 host capacity，可选择在空间不足但达到 threshold 时截断。
 *
 * prefetch 使用 allow_truncate=true，以模拟 best-effort/threshold-sized prefix；write backup
 * 使用 false，避免 partial host backup 破坏 node-level residency 语义。
 */
HiCacheState::HostAllocationResult HiCacheState::request_host_allocation(const HiCacheFact & fact, HiCacheSummary & summary,
                                                                         std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                                                                         uint64_t requested_pages, uint64_t minimum_pages, bool allow_truncate,
                                                                         const std::string & reason) {
    auto result = HostAllocationResult{
        .requested_pages = requested_pages,
        .capacity_pages = policy_.l2_capacity_pages(),
    };
    if (requested_pages == 0) {
        result.accepted = true;
        return result;
    }
    if (result.capacity_pages == 0) {
        result.accepted = true;
        result.accepted_pages = requested_pages;
        return result;
    }

    enforce_host_capacity(fact, summary, transitions, scope, requested_pages);
    sync_capacity(scope, normalized_scope(fact), {}, reason + "_host_allocation_post_cleanup");
    const auto snapshot = scope.capacity.snapshot();
    result.occupied_pages = snapshot.occupied_host_pages;
    result.reserved_pages = snapshot.reserved_host_pages;
    const auto committed_pages = snapshot.occupied_host_pages + snapshot.reserved_host_pages;
    const auto available_pages = committed_pages < result.capacity_pages ? result.capacity_pages - committed_pages : uint64_t{ 0 };
    if (available_pages >= requested_pages) {
        result.accepted = true;
        result.accepted_pages = requested_pages;
        return result;
    }
    if (allow_truncate && available_pages >= minimum_pages) {
        result.accepted = true;
        result.truncated = true;
        result.accepted_pages = available_pages;
    }
    return result;
}


} // namespace markov::trace_graph::modules::hicache::model
