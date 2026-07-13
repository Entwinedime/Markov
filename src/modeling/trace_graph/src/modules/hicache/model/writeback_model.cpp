/**
 * @file
 * @brief Models HiCache write-through/write-back backup and acknowledgement.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

/**
 * @brief Releases temporary write-through references at control or finalization.
 *
 * Selective write-through holds an ordinary lock reference until CPU-write
 * acknowledgement. The model drains it at the next target-control fact, or at final
 * state when no later fact exists, preventing temporary locks from surviving replay.
 */
void HiCacheState::drain_write_through_backup_refs(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                   ScopedState & scope, const std::string & reason) {
    if (scope.pending_write_through_backups.empty()) return;

    auto pending = std::exchange(scope.pending_write_through_backups, {});
    for (const auto & backup : pending) {
        const bool source_available = fact.event_name != "hicache_finalize";
        const auto consumer_epoch = source_available ? scope.clock.record_fact_boundary(normalized_scope(fact),
                                                                                        scoped_request_key(fact),
                                                                                        "write_through_capacity_consumer",
                                                                                        fact.source_event_index,
                                                                                        fact.ts)
                                                     : scope.clock.record_target_finalize_boundary(normalized_scope(fact), fact.ts);
        scope.async_ops.set_storage_consumer_boundary(backup.storage_operation_id,
                                                      consumer_epoch,
                                                      fact.ts,
                                                      fact.source_node_id,
                                                      fact.source_event_index,
                                                      fact.role,
                                                      source_available);
        const auto before = debug_state_digest();
        const auto ref = scope.refs.release_owner(scope.tree, backup.owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, reason);
        if constexpr (debug_records_enabled())
            record_transition(fact,
                              summary,
                              transitions,
                              TransitionDescriptor{ .kind = "complete_write_through_backup", .tier = "writeback" },
                              backup.pages,
                              before);
    }
}

bool HiCacheState::reserve_host_backup_capacity(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                                const std::vector<std::string> & pages, uint64_t allocation_pages) {
    if (allocation_pages == 0) return true;
    const auto allocation = request_host_allocation(fact,
                                                    summary,
                                                    transitions,
                                                    scope,
                                                    HostAllocationRequest{
                                                        .requested_pages = allocation_pages,
                                                        .minimum_pages = allocation_pages,
                                                        .allow_truncate = false,
                                                        .reason = "write_backup",
                                                    });
    if constexpr (debug_records_enabled()) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "host_allocation",
                                   .policy_name = "write_backup",
                                   .decision = allocation.accepted ? "accept_host_backup_pages" : "skip_host_backup_capacity",
                                   .reason = allocation.accepted ? "target host pool can fit write_backup allocation after SGLang-style cleanup"
                                                                 : "target host pool still lacks space after SGLang-style cleanup",
                                   .accepted = allocation.accepted,
                                   .requested_pages = allocation_pages,
                                   .candidate_pages = allocation.accepted_pages,
                                   .capacity_pages = allocation.capacity_pages,
                                   .occupied_pages = allocation.occupied_pages,
                                   .reserved_pages = allocation.reserved_pages,
                                   .pages = pages,
                               });
    }
    return allocation.accepted;
}

std::string HiCacheState::begin_storage_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                               HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                               const std::vector<std::string> & device_to_host_pages) {
    const auto storage_id = scope.clock.next_operation_id("storage");
    const auto request_key = scoped_request_key(fact);
    const auto storage_owner = request_key + ":storage:" + storage_id;
    const auto before_enqueue = debug_state_digest();
    scope.async_ops.insert_storage(HiCacheStorageOperation{
        .header = make_operation_header(HiCacheOperationKind::Storage, storage_id, fact, normalized_scope(fact), request_key, storage_owner, pages, 0),
        .device_to_host_pages = device_to_host_pages,
        .capacity_gate_pages = {},
    });
    const auto ref = scope.refs.acquire_host(scope.tree, storage_owner, "storage", request_key, storage_id, std::vector<HiCacheNodeId>{ node_id });
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_acquire");
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "enqueue_storage_backup", .tier = "storage" }, pages, before_enqueue);
    return storage_id;
}

void HiCacheState::materialize_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                           HiCacheNodeId node_id, const std::vector<std::string> & pages, const std::string & storage_id,
                                           bool storage_readable) {
    const auto before = debug_state_digest();
    scope.tree.mark_host_visible(node_id, storage_readable);
    scope.tree.clear_dirty(node_id);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "commit_host_backup");
    if (storage_readable) {
        scope.storage.mark_readable_pages(normalized_scope(fact), pages);
#ifdef DEBUG
        scope.storage.mark_materialized_pages(pages, node_id);
#endif
    }
    if constexpr (debug_records_enabled()) {
        record_transition(fact,
                          summary,
                          transitions,
                          TransitionDescriptor{
                              .kind = storage_readable ? "commit_host_storage_backup" : "commit_host_backup",
                              .tier = "L2",
                          },
                          pages,
                          before);
    }
    if (storage_readable && policy_.write_count_enabled()) {
        scope.async_ops.set_storage_capacity_gate_pages(storage_id, pages);
        hold_write_through_backup_ref(fact, summary, transitions, scope, node_id, pages, storage_id);
    }
}

void HiCacheState::complete_storage_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                           const std::string & storage_id, const std::vector<std::string> & pages) {
    if (storage_id.empty()) return;
    const auto before_complete = debug_state_digest();
    scope.async_ops.set_storage_state(storage_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, scoped_request_key(fact) + ":storage:" + storage_id);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_release");
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "complete_storage_backup", .tier = "storage" }, pages, before_complete);
}

/**
 * @brief Commits one device node value as a host and optional storage backup.
 *
 * `storage_readable` also registers the backup in the readable L3 directory. Dirty
 * write-back eviction and hit-count write-through share this path, and both must pass
 * target host-capacity cleanup before materialization.
 */
bool HiCacheState::commit_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                      HiCacheNodeId node_id, bool storage_readable) {
    const auto * node = scope.tree.node(node_id);
    if (node == nullptr) return false;
    const auto & pages = scope.tree.node_pages(node_id);
    if (pages.empty()) return false;
    const auto host_allocation_pages = !(node->residency.host_present && node->residency.host_visible) ? static_cast<uint64_t>(pages.size()) : uint64_t{ 0 };
    if (!reserve_host_backup_capacity(fact, summary, transitions, scope, pages, host_allocation_pages)) return false;
    const bool device_to_host_required = node->residency.device_present && (node->residency.device_dirty || !node->residency.host_visible);
    const auto device_to_host_pages = device_to_host_required ? pages : std::vector<std::string>{};
    const auto storage_id = storage_readable ? begin_storage_backup(fact, summary, transitions, scope, node_id, pages, device_to_host_pages) : std::string{};
    materialize_host_backup(fact, summary, transitions, scope, node_id, pages, storage_id, storage_readable);
    complete_storage_backup(fact, summary, transitions, scope, storage_id, pages);
    return true;
}

/**
 * @brief Holds an ordinary lock reference until write-through acknowledgement.
 *
 * This reference does not represent request lifecycle. It approximates the interval
 * during which a backup node remains protected before SGLang `writing_check()`.
 */
void HiCacheState::hold_write_through_backup_ref(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                                 HiCacheNodeId node_id, const std::vector<std::string> & pages, const std::string & storage_operation_id) {
    const auto chain = scope.tree.ancestor_node_ids(node_id);
    if (chain.empty()) return;

    const auto operation_id = scope.clock.next_operation_id("write_through_backup");
    const auto owner = scoped_request_key(fact) + ":write_through_backup:" + operation_id;
    const auto before = debug_state_digest();
    const auto ref = scope.refs.acquire_lock(scope.tree, owner, "write_through_backup", scoped_request_key(fact), operation_id, chain);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "write_through_backup_ref_acquire");
    scope.pending_write_through_backups.push_back(PendingWriteThroughBackup{
        .owner = owner,
        .storage_operation_id = storage_operation_id,
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
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "enqueue_write_through_backup", .tier = "writeback" }, pages, before);
}

void HiCacheState::record_write_count_skip(const HiCacheFact & fact, const std::vector<std::string> & pages, const std::string & reason) {
    if constexpr (debug_records_enabled()) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "write_policy",
                                   .policy_name = policy_.write_policy(),
                                   .decision = "skip_hit_count_backup",
                                   .reason = reason,
                                   .accepted = false,
                                   .candidate_pages = static_cast<uint64_t>(pages.size()),
                                   .pages = pages,
                               });
    }
    else {
        (void)fact;
        (void)pages;
        (void)reason;
    }
}

void HiCacheState::apply_write_count_to_node(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                             const WriteCountRequest & request) {
    const auto node_id = request.node_id;
    const auto threshold = request.threshold;
    auto * node = scope.tree.mutable_node(node_id);
    if (node == nullptr || !node->residency.device_present) return;
    const auto before = debug_state_digest();
    (void)core::checked_increment_u64(node->hit_count, "HiCache radix node hit count exceeds uint64 range");
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "increment_hit_count", .tier = "hit_count" }, node->pages, before);
    const auto should_backup = !has_host_backup(*node) && node->hit_count >= threshold;
    if constexpr (debug_records_enabled()) {
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
    }
    if (should_backup) (void)commit_host_backup(fact, summary, transitions, scope, node_id, true);
}

/**
 * @brief Applies hit-count write-through policy to nodes on a request path.
 *
 * Write-back bypasses this path. Selective write-through commits host/storage backup
 * when node hit count reaches the threshold and holds protection until acknowledgement.
 */
void HiCacheState::apply_write_count_policy(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                            const std::vector<std::string> & pages) {
    if (!policy_.write_count_enabled()) {
        record_write_count_skip(fact, pages, "target write policy does not use hit-count backup");
        return;
    }
    const auto threshold = policy_.write_through_threshold();
    if (threshold == 0) {
        record_write_count_skip(fact, pages, "resolved write-through threshold is zero");
        return;
    }

    const auto lookup = scope.tree.lookup_peek(pages);
    for (const auto node_id : lookup.topology_chain) {
        apply_write_count_to_node(fact,
                                  summary,
                                  transitions,
                                  scope,
                                  WriteCountRequest{
                                      .node_id = node_id,
                                      .threshold = threshold,
                                  });
    }
}


} // namespace markov::trace_graph::modules::hicache::model
