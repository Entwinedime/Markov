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
void HiCacheState::drain_write_through_backup_refs(const HiCacheFact & fact, ScopedState & scope, const std::string & reason) {
    if (scope.pending_write_through_backups.empty()) return;

    auto pending = std::exchange(scope.pending_write_through_backups, {});
    for (const auto & backup : pending) {
        auto * operation = scope.async_ops.storage_operation(backup.storage_operation_id);
        const bool force_finalize = fact.event_name == "hicache_finalize";
        if (!force_finalize && operation != nullptr && operation->device_to_host_schedule.available && operation->device_to_host_schedule.ready_ts > fact.ts) {
            scope.pending_write_through_backups.push_back(backup);
            continue;
        }
        if (operation != nullptr && force_finalize) {
            // The write-through policy already made this storage write mandatory when the
            // backup was enqueued.  A trace-window boundary can precede the asynchronous
            // acknowledgement, but it must not erase the target-derived payload shape.
            // Settle the required pages without claiming a source-observed completion or
            // extending the executable E2E window; the finalize consumer below remains a
            // target-only boundary with no source anchor.
            operation->host_to_storage_pages = backup.pages;
            scope.async_ops.set_storage_state(backup.storage_operation_id, HiCacheOperationState::Committed, "target_finalize_required_write_through", fact.ts);
            const auto storage_ref = scope.refs.release_owner(scope.tree, operation->header.owner);
            sync_capacity_for_ref(scope, normalized_scope(fact), storage_ref, "storage_ref_release_at_window_end");
        }
        else if (operation != nullptr) {
            operation->host_to_storage_pages = backup.pages;
            complete_storage_backup(fact, scope, backup.storage_operation_id, backup.pages);
        }
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
                                                      fact.execution_anchor_node_id,
                                                      fact.source_event_index,
                                                      fact.role,
                                                      source_available);
        const auto ref = scope.refs.release_owner(scope.tree, backup.owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, reason);
    }
}

bool HiCacheState::reserve_host_backup_capacity(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages,
                                                uint64_t allocation_pages) {
    if (allocation_pages == 0) return true;
    const auto allocation = request_host_allocation(fact,
                                                    scope,
                                                    HostAllocationRequest{
                                                        .requested_pages = allocation_pages,
                                                        .minimum_pages = allocation_pages,
                                                        .allow_truncate = false,
                                                        .reason = "write_backup",
                                                    });
    (void)pages;
    return allocation.accepted;
}

std::string HiCacheState::begin_storage_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                               const std::vector<std::string> & device_to_host_pages) {
    const auto storage_id = scope.clock.next_operation_id("storage");
    const auto request_key = scoped_request_key(fact);
    const auto storage_owner = request_key + ":storage:" + storage_id;
    const auto device_to_host_schedule = schedule_target_io(scope, TargetIoLane::DeviceToHost, fact.ts, static_cast<uint64_t>(device_to_host_pages.size()));
    std::vector<std::string> existing_storage_pages;
    std::vector<std::string> new_storage_pages;
    existing_storage_pages.reserve(pages.size());
    new_storage_pages.reserve(pages.size());
    for (const auto & page : pages) {
        auto & destination = scope.storage.readable(page) ? existing_storage_pages : new_storage_pages;
        destination.push_back(page);
    }
    scope.async_ops.insert_storage(HiCacheStorageOperation{
        .header = make_operation_header(HiCacheOperationKind::Storage, storage_id, fact, normalized_scope(fact), request_key, storage_owner, pages, 0),
        .device_to_host_pages = device_to_host_pages,
        .host_to_storage_pages = policy_.write_count_enabled() ? std::vector<std::string>{} : pages,
        .host_to_storage_existing_pages = std::move(existing_storage_pages),
        .host_to_storage_new_pages = std::move(new_storage_pages),
        .capacity_gate_pages = {},
        .device_to_host_schedule = device_to_host_schedule,
    });
    if (!policy_.write_count_enabled()) {
        const auto ref = scope.refs.acquire_host(scope.tree, storage_owner, "storage", request_key, storage_id, std::vector<HiCacheNodeId>{ node_id });
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_acquire");
    }
    return storage_id;
}

void HiCacheState::materialize_host_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                           const std::string & storage_id, bool storage_readable) {
    scope.tree.mark_host_visible(node_id, storage_readable);
    scope.tree.clear_dirty(node_id);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "commit_host_backup");
    if (storage_readable) {
        for (const auto & page : pages) {
            scope.storage_access_bytes_completed = core::checked_add_u64(scope.storage_access_bytes_completed,
                                                                         config_.kv_bytes_per_page,
                                                                         "HiCache cumulative storage-access bytes exceed uint64 range");
            scope.storage_page_last_access_end_byte[page] = scope.storage_access_bytes_completed;
        }
        scope.storage.mark_readable_pages(normalized_scope(fact), pages);
    }
}

void HiCacheState::complete_storage_backup(const HiCacheFact & fact, ScopedState & scope, const std::string & storage_id,
                                           const std::vector<std::string> & pages) {
    if (storage_id.empty()) return;
    scope.async_ops.set_storage_state(storage_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, scoped_request_key(fact) + ":storage:" + storage_id);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_release");
    (void)pages;
}

/**
 * @brief Commits one device node value as a host and optional storage backup.
 *
 * `storage_readable` also registers the backup in the readable L3 directory. Dirty
 * write-back eviction and hit-count write-through share this path, and both must pass
 * target host-capacity cleanup before materialization.
 */
bool HiCacheState::commit_host_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, bool storage_readable) {
    const auto * node = scope.tree.node(node_id);
    if (node == nullptr) return false;
    const auto & pages = scope.tree.node_pages(node_id);
    if (pages.empty()) return false;
    const auto host_allocation_pages = !(node->residency.host_present && node->residency.host_visible) ? static_cast<uint64_t>(pages.size()) : uint64_t{ 0 };
    if (!reserve_host_backup_capacity(fact, scope, pages, host_allocation_pages)) return false;
    const bool device_to_host_required = node->residency.device_present && (node->residency.device_dirty || !node->residency.host_visible);
    const auto device_to_host_pages = device_to_host_required ? pages : std::vector<std::string>{};
    const auto storage_id = storage_readable ? begin_storage_backup(fact, scope, node_id, pages, device_to_host_pages) : std::string{};
    materialize_host_backup(fact, scope, node_id, pages, storage_id, storage_readable);
    if (storage_readable && policy_.write_count_enabled()) hold_write_through_backup_ref(fact, scope, node_id, pages, storage_id);
    else complete_storage_backup(fact, scope, storage_id, pages);
    return true;
}

/**
 * @brief Holds an ordinary lock reference until write-through acknowledgement.
 *
 * This reference does not represent request lifecycle. It approximates the interval
 * during which a backup node remains protected before SGLang `writing_check()`.
 */
void HiCacheState::hold_write_through_backup_ref(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                                 const std::string & storage_operation_id) {
    const auto chain = scope.tree.ancestor_node_ids(node_id);
    if (chain.empty()) return;

    const auto operation_id = scope.clock.next_operation_id("write_through_backup");
    const auto owner = scoped_request_key(fact) + ":write_through_backup:" + operation_id;
    const auto ref = scope.refs.acquire_lock(scope.tree, owner, "write_through_backup", scoped_request_key(fact), operation_id, chain);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "write_through_backup_ref_acquire");
    scope.pending_write_through_backups.push_back(PendingWriteThroughBackup{
        .owner = owner,
        .storage_operation_id = storage_operation_id,
        .pages = pages,
    });
}

void HiCacheState::apply_write_count_to_node(const HiCacheFact & fact, ScopedState & scope, const WriteCountRequest & request) {
    const auto node_id = request.node_id;
    const auto threshold = request.threshold;
    auto * node = scope.tree.mutable_node(node_id);
    if (node == nullptr || !node->residency.device_present) return;
    (void)core::checked_increment_u64(node->hit_count, "HiCache radix node hit count exceeds uint64 range");
    const auto should_backup = !has_host_backup(*node) && node->hit_count >= threshold;
    if (should_backup) (void)commit_host_backup(fact, scope, node_id, true);
}

/**
 * @brief Applies hit-count write-through policy to nodes on a request path.
 *
 * Write-back bypasses this path. Selective write-through commits host/storage backup
 * when node hit count reaches the threshold and holds protection until acknowledgement.
 */
void HiCacheState::apply_write_count_policy(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages) {
    if (!policy_.write_count_enabled()) {
        return;
    }
    const auto threshold = policy_.write_through_threshold();
    if (threshold == 0) {
        return;
    }

    const auto lookup = scope.tree.lookup_peek(pages);
    for (const auto node_id : lookup.topology_chain) {
        apply_write_count_to_node(fact,
                                  scope,
                                  WriteCountRequest{
                                      .node_id = node_id,
                                      .threshold = threshold,
                                  });
    }
}


} // namespace markov::trace_graph::modules::hicache::model
