/**
 * @file
 * @brief Models HiCache host capacity, storage backup, and device eviction.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

void HiCacheState::record_device_eviction_policy(const HiCacheFact & fact, const radix::HiCacheCacheNode & node, bool needs_writeback) {
    if constexpr (debug_records_enabled()) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "write_policy",
                                   .policy_name = policy_.write_policy(),
                                   .decision = needs_writeback ? "enqueue_dirty_eviction_writeback" : "evict_without_writeback",
                                   .reason = needs_writeback               ? "write_back dirty device node must refresh host backup before eviction"
                                             : node.residency.device_dirty ? "target write policy does not require dirty eviction writeback"
                                                                           : "device node is clean at eviction boundary",
                                   .accepted = needs_writeback,
                                   .candidate_pages = static_cast<uint64_t>(node.pages.size()),
                                   .pages = node.pages,
                               });
    }
    else {
        (void)fact;
        (void)node;
        (void)needs_writeback;
    }
}

bool HiCacheState::commit_device_eviction_writeback(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                    ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages) {
#ifdef DEBUG
    (void)core::checked_increment_u64(summary.dirty_eviction_events, "HiCache dirty eviction count exceeds uint64 range");
#endif
    const auto writeback_id = scope.clock.next_operation_id("writeback");
    const auto writeback_owner = scoped_request_key(fact) + ":writeback:" + writeback_id;
    const auto before_enqueue = debug_state_digest();
    scope.async_ops.insert_writeback(HiCacheWritebackOperation{
        .header = make_operation_header(HiCacheOperationKind::Writeback,
                                        writeback_id,
                                        normalized_scope(fact),
                                        scoped_request_key(fact),
                                        writeback_owner,
                                        pages,
                                        fact.ts,
                                        0),
    });
    auto ref = scope.refs.acquire_lock(scope.tree, writeback_owner, "writeback", scoped_request_key(fact), writeback_id, std::vector<HiCacheNodeId>{ node_id });
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "writeback_ref_acquire");
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "enqueue_writeback", .tier = "writeback" }, pages, before_enqueue);

    const auto committed = commit_host_backup(fact, summary, transitions, scope, node_id, true);
    const auto before_complete = debug_state_digest();
    scope.async_ops.set_writeback_state(writeback_id,
                                        committed ? HiCacheOperationState::Committed : HiCacheOperationState::Cancelled,
                                        committed ? "sync_commit" : "host_backup_capacity_rejected",
                                        fact.ts);
    ref = scope.refs.release_owner(scope.tree, writeback_owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "writeback_ref_release");
    if constexpr (debug_records_enabled()) {
        record_transition(fact,
                          summary,
                          transitions,
                          TransitionDescriptor{
                              .kind = committed ? "complete_writeback" : "cancel_writeback",
                              .tier = "writeback",
                          },
                          pages,
                          before_complete);
    }
    return committed;
}

uint64_t HiCacheState::release_device_residency(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                                HiCacheNodeId node_id, const std::vector<std::string> & pages) {
    const auto released_pages = static_cast<uint64_t>(pages.size());
    const auto before = debug_state_digest();
    const auto * node = scope.tree.node(node_id);
    if (node != nullptr && has_host_backup(*node)) scope.tree.demote_device_to_host(node_id, false);
    else scope.tree.remove_device_regular(node_id);
    const auto allocator_released_pages = scope.device_allocator.release(released_pages);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "evict_device_node");
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "evict_l1_node", .tier = "L1" }, pages, before);
    if constexpr (debug_records_enabled()) {
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
    }
    else { (void)allocator_released_pages; }
    return released_pages;
}

/**
 * @brief Evicts one device node, committing dirty write-back data first.
 *
 * Device eviction updates radix residency, allocator release state, and the capacity
 * index. Dirty write-back acknowledgement is currently synchronous, while operation
 * and reference lifecycles remain explicit in Debug transition evidence.
 */
uint64_t HiCacheState::evict_device_node(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                         HiCacheNodeId node_id) {
    auto * node = scope.tree.mutable_node(node_id);
    if (node == nullptr || !node->residency.device_present) return 0;

    const auto pages = node->pages;
    const bool needs_writeback = policy_.write_back_enabled() && node->residency.device_dirty;
    record_device_eviction_policy(fact, *node, needs_writeback);
    if (needs_writeback && !commit_device_eviction_writeback(fact, summary, transitions, scope, node_id, pages)) return 0;
    return release_device_residency(fact, summary, transitions, scope, node_id, pages);
}

/**
 * @brief Attempts to evict one host leaf under SGLang rules.
 *
 * Host cleanup skips leaves with a positive host-reference count. Successful eviction
 * removes the host leaf or subtree rather than merely clearing `node.host_value`.
 */
uint64_t HiCacheState::evict_host_node(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                       HiCacheNodeId node_id) {
    const auto * node = scope.tree.node(node_id);
    const auto pages = node == nullptr ? std::vector<std::string>{} : node->pages;
    if (node != nullptr && node->refs.host_ref_total > 0) {
        if constexpr (debug_records_enabled())
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

    const auto before = debug_state_digest();
    const auto result = scope.tree.evict_host_leaf(node_id);
    sync_capacity(scope,
                  normalized_scope(fact),
                  result.affected_nodes.empty() ? std::vector<HiCacheNodeId>{ node_id } : result.affected_nodes,
                  result.evicted ? "evict_host_leaf" : "evict_host_leaf_skipped");
    if constexpr (debug_records_enabled())
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
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "evict_host_node", .tier = "L2" }, pages, before);
    return static_cast<uint64_t>(pages.size());
}

/**
 * @brief Enforces device cleanup through the SGLang allocator availability gate.
 *
 * Cleanup is not driven directly by final occupancy. The model reconstructs
 * `available_size` before allocation and passes the full request budget to radix
 * eviction only when availability is insufficient.
 */
void HiCacheState::enforce_device_capacity(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                           uint64_t requested_pages) {
    ensure_device_allocator(scope);
    const auto capacity = scope.device_allocator.capacity_pages;
    if (capacity == 0 || requested_pages == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, "device_allocator_budget");
    if (!scope.device_allocator.should_evict(requested_pages)) {
        if constexpr (debug_records_enabled())
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
    if constexpr (debug_records_enabled())
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
        const auto victim = scope.capacity.select_device_victim(runtime::HiCacheVictimRequest{
            .capacity_pages = capacity,
            .requested_pages = requested_pages,
            .reason = "device_allocator_loop",
        });
        if (!victim) break;
        const auto released_pages = evict_device_node(fact, summary, transitions, scope, *victim);
        if (released_pages == 0 || released_pages >= target) break;
        target -= released_pages;
    }
}

/**
 * @brief Enforces host cleanup at allocation or capacity-audit boundaries.
 *
 * A positive request drives cleanup with the allocation budget; a zero request removes
 * only existing excess. Reserved host pages contribute pressure so active and pending
 * prefetches cannot be ignored.
 */
void HiCacheState::enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                         uint64_t requested_pages) {
    const auto capacity = policy_.l2_capacity_pages();
    if (capacity == 0) return;
    const auto budget_reason = requested_pages > 0 ? "host_allocation_request_budget" : "host_capacity_budget";
    const auto loop_reason = requested_pages > 0 ? "host_allocation_request_loop" : "host_capacity_loop";
    sync_capacity(scope, normalized_scope(fact), {}, budget_reason);
    const auto snapshot = scope.capacity.snapshot();
    auto target = allocation_cleanup_target(HostCleanupInput{
        .occupied_pages = snapshot.occupied_host_pages,
        .reserved_pages = snapshot.reserved_host_pages,
        .capacity_pages = capacity,
        .requested_pages = requested_pages,
    });
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {}, loop_reason);
        const auto victim = scope.capacity.select_host_victim(runtime::HiCacheVictimRequest{
            .capacity_pages = capacity,
            .requested_pages = requested_pages,
            .reason = loop_reason,
        });
        if (!victim) break;
        const auto victim_pages = evict_host_node(fact, summary, transitions, scope, *victim);
        if (victim_pages == 0) break;
        if (victim_pages >= target) break;
        target -= victim_pages;
    }
}

/**
 * @brief Requests host capacity with optional threshold-preserving truncation.
 *
 * Prefetch permits truncation for best-effort threshold-sized prefixes. Write backup
 * rejects truncation because partial backup would violate node-level residency semantics.
 */
HiCacheState::HostAllocationResult HiCacheState::request_host_allocation(const HiCacheFact & fact, HiCacheSummary & summary,
                                                                         HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                                                         const HostAllocationRequest & request) {
    auto result = HostAllocationResult{
        .requested_pages = request.requested_pages,
        .capacity_pages = policy_.l2_capacity_pages(),
    };
    if (request.requested_pages == 0) {
        result.accepted = true;
        return result;
    }
    if (result.capacity_pages == 0) {
        result.accepted = true;
        result.accepted_pages = request.requested_pages;
        return result;
    }

    enforce_host_capacity(fact, summary, transitions, scope, request.requested_pages);
    sync_capacity(scope, normalized_scope(fact), {}, std::string(request.reason) + "_host_allocation_post_cleanup");
    const auto snapshot = scope.capacity.snapshot();
    result.occupied_pages = snapshot.occupied_host_pages;
    result.reserved_pages = snapshot.reserved_host_pages;
    const auto committed_pages =
        core::checked_add_u64(snapshot.occupied_host_pages, snapshot.reserved_host_pages, "HiCache host allocation committed pages exceed uint64 range");
    const auto available_pages = committed_pages < result.capacity_pages ? result.capacity_pages - committed_pages : uint64_t{ 0 };
    if (available_pages >= request.requested_pages) {
        result.accepted = true;
        result.accepted_pages = request.requested_pages;
        return result;
    }
    if (request.allow_truncate && available_pages >= request.minimum_pages) {
        result.accepted = true;
        result.truncated = true;
        result.accepted_pages = available_pages;
    }
    return result;
}


} // namespace markov::trace_graph::modules::hicache::model
