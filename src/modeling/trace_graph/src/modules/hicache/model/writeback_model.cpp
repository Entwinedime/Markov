/**
 * @file
 * @brief Models HiCache write-through/write-back backup and acknowledgement.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {
namespace {
std::vector<HiCacheNodeId> backup_nodes(const HiCacheTokenRadixTree & tree, const std::vector<std::string> & pages) {
    std::set<HiCacheNodeId> nodes;
    for (const auto & page : pages) {
        const auto node = tree.node_for_page(page);
        if (!node) throw std::logic_error("Pending backup lost its protected radix page");
        nodes.insert(*node);
    }
    return { nodes.begin(), nodes.end() };
}
} // namespace

std::vector<HiCacheDeviceWrite> HiCacheState::pending_device_writes(const HiCacheFact & fact) const {
    std::vector<HiCacheDeviceWrite> writes;
    const auto found = scopes_.find(normalized_scope(fact));
    if (found == scopes_.end()) return writes;
    for (const auto & [id, operation] : found->second.async_ops.storage_ops())
        if (!operation.device_completed_at && !operation.host_materialized)
            writes.push_back({ operation.header, operation.device_to_host_schedule, operation.node_id });
    std::ranges::sort(writes, {}, [](const auto & write) { return write.header.enqueue_epoch; });
    return writes;
}

std::vector<HiCacheDeviceWrite> HiCacheState::unacknowledged_device_writes(const HiCacheFact & fact) const {
    std::vector<HiCacheDeviceWrite> writes;
    const auto found = scopes_.find(normalized_scope(fact));
    if (found == scopes_.end()) return writes;
    for (const auto & pending : found->second.pending_write_through_backups) {
        const auto & operation = found->second.async_ops.storage_ops().at(pending.storage_operation_id);
        writes.push_back({ operation.header, operation.device_to_host_schedule, operation.node_id });
    }
    return writes;
}

void HiCacheState::complete_device_write(const HiCacheFact & fact, const std::string & operation_id) {
    auto * operation = scope_state(fact).async_ops.storage_operation(operation_id);
    if (!execution_prefetch_control_ || !operation || operation->device_completed_at
        || fact.ts < operation->device_to_host_schedule.ready_ts)
        throw std::logic_error("Write completion requires a causal, uncompleted submitted DMA");
    operation->device_completed_at = fact.ts;
}

uint64_t HiCacheState::write_completion_count(const HiCacheFact & fact) const {
    if (!execution_prefetch_control_) throw std::logic_error("Explicit write confirmation requires execution-driven state");
    const auto found = scopes_.find(normalized_scope(fact));
    if (found == scopes_.end()) return 0;
    uint64_t count = 0;
    for (const auto & pending : found->second.pending_write_through_backups) {
        const auto & operation = found->second.async_ops.storage_ops().at(pending.storage_operation_id);
        if (!operation.device_completed_at || *operation.device_completed_at > fact.ts) break;
        ++count;
    }
    return count;
}

void HiCacheState::acknowledge_writes(const HiCacheFact & fact, uint64_t batches) {
    if (batches > write_completion_count(fact)) throw std::logic_error("Write confirmation exceeds the completed DMA prefix");
    auto & scope = scope_state(fact);
    advance_storage_backups(fact, scope); // Classify storage keys at actual submission, after earlier writes have completed.
    drain_write_through_backup_refs(fact, scope, batches);
}

/**
 * @brief Releases temporary write-through references at control or finalization.
 *
 * Selective write-through holds an ordinary lock reference until CPU-write
 * acknowledgement. Execution requires an explicit sampled batch count; the batch
 * model retains its next-control/finalization approximation until CLI migration.
 */
void HiCacheState::drain_write_through_backup_refs(const HiCacheFact & fact, ScopedState & scope, std::optional<uint64_t> acknowledged_batches) {
    if (execution_prefetch_control_ && !acknowledged_batches) return;
    if (scope.pending_write_through_backups.empty()) return;

    auto pending = std::exchange(scope.pending_write_through_backups, {});
    uint64_t position = 0;
    for (const auto & backup : pending) {
        if (acknowledged_batches && position++ >= *acknowledged_batches) {
            scope.pending_write_through_backups.push_back(backup);
            continue;
        }
        auto * operation = scope.async_ops.storage_operation(backup.storage_operation_id);
        const bool force_finalize = fact.event_name == "hicache_finalize";
        if (!force_finalize && operation != nullptr
            && (execution_prefetch_control_ ? !operation->device_completed_at || *operation->device_completed_at > fact.ts
                                           : operation->device_to_host_schedule.available && operation->device_to_host_schedule.ready_ts > fact.ts)) {
            scope.pending_write_through_backups.push_back(backup);
            continue;
        }
        if (operation != nullptr && !operation->host_materialized)
            materialize_host_backup(fact, scope, operation->node_id, backup.pages, backup.storage_operation_id);
        const bool waiting_for_capacity_return = operation != nullptr && !operation->capacity_gate_pages.empty()
                                                  && operation->header.source_node_id == fact.source_node_id && allocation_pending(fact);
        if (operation != nullptr && operation->header.consumer_epoch == 0 && !waiting_for_capacity_return) {
            const bool source_available = fact.event_name != "hicache_finalize";
            const auto consumer_epoch = scope.clock.next_boundary_epoch();
            scope.async_ops.set_storage_consumer_boundary(backup.storage_operation_id,
                                                          consumer_epoch,
                                                          force_finalize ? operation->device_to_host_schedule.ready_ts : fact.ts,
                                                          fact.source_node_id,
                                                          fact.execution_anchor_node_id,
                                                          fact.source_event_index,
                                                          fact.role,
                                                          source_available);
        }
        if (!backup.owner.empty()) {
            const auto ref = scope.refs.release_owner(scope.tree, backup.owner);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        }
    }
}

void HiCacheState::advance_storage_backups(const HiCacheFact & fact, ScopedState & scope, bool force) {
    std::vector<std::string> completed;
    for (auto & [operation_id, operation] : scope.async_ops.storage_ops()) {
        if (!execution_prefetch_control_ && !operation.host_materialized && operation.device_to_host_schedule.available
            && (force || operation.device_to_host_schedule.ready_ts <= fact.ts))
            materialize_host_backup(fact, scope, operation.node_id, operation.header.pages, operation_id);
        if (!operation.storage_committed && operation.host_to_storage_schedule.available
            && (force || operation.host_to_storage_schedule.ready_ts <= fact.ts))
            completed.push_back(operation_id);
    }
    // The backend queue is FIFO. A later observation may discover several
    // completions at once; unordered_map iteration is not completion order.
    if (execution_prefetch_control_)
        std::ranges::sort(completed, [&](const auto & a, const auto & b) {
            const auto & left = scope.async_ops.storage_ops().at(a);
            const auto & right = scope.async_ops.storage_ops().at(b);
            return std::tuple{ left.host_to_storage_schedule.ready_ts, left.header.enqueue_epoch }
                   < std::tuple{ right.host_to_storage_schedule.ready_ts, right.header.enqueue_epoch };
        });
    for (const auto & operation_id : completed) complete_storage_backup(fact, scope, operation_id);
}

bool HiCacheState::reserve_host_backup_capacity(const HiCacheFact & fact, ScopedState & scope, uint64_t allocation_pages) {
    if (allocation_pages == 0) return true;

    const auto allocation = request_host_allocation(fact,
                                                    scope,
                                                    HostAllocationRequest{
                                                        .requested_pages = allocation_pages,
                                                        .minimum_pages = allocation_pages,
                                                        .allow_truncate = false,
                                                    });
    return allocation.accepted;
}

std::string HiCacheState::begin_storage_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                               const std::vector<std::string> & device_to_host_pages) {
    const auto storage_id = scope.clock.next_operation_id("storage");
    const auto request_key = scoped_request_key(fact);
    const auto storage_owner = request_key + ":storage:" + storage_id;
    const auto device_to_host_schedule = schedule_target_io(normalized_scope(fact), "write_device_to_host", fact.ts, device_to_host_pages.size());
    HiCacheStorageOperation operation{
        .header = make_operation_header(storage_id, fact, normalized_scope(fact), request_key, storage_owner, pages, scope.clock.next_enqueue_epoch()),
        .node_id = node_id,
        .device_to_host_pages = device_to_host_pages,
        .device_to_host_schedule = device_to_host_schedule,
    };
    if (!execution_prefetch_control_)
        submit_storage_backup(fact, scope, operation, device_to_host_schedule.available ? device_to_host_schedule.ready_ts : fact.ts);
    else {
        auto & residency = scope.tree.mutable_node(node_id)->residency;
        operation.reserved_host_pages = residency.host_visible ? 0 : pages.size();
        residency.host_present = true; // Prevent a second write while the first DMA is pending.
    }
    scope.async_ops.insert_storage(std::move(operation));
    const auto ref = scope.refs.acquire_host(scope.tree, storage_owner, std::vector<HiCacheNodeId>{ node_id });
    sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    return storage_id;
}

void HiCacheState::submit_storage_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheStorageOperation & operation, uint64_t submitted_at) {
    const auto & pages = operation.header.pages;
    std::vector<std::string> existing_storage_pages;
    std::vector<std::string> new_storage_pages;
    existing_storage_pages.reserve(pages.size());
    new_storage_pages.reserve(pages.size());
    for (const auto & page : pages) {
        auto & destination = scope.storage.readable(page) ? existing_storage_pages : new_storage_pages;
        destination.push_back(page);
    }
    const auto batch_limit = config_.io_cost.storage_batch_pages;
    std::vector<uint64_t> existing_batch_pages(pages.size() / batch_limit + static_cast<size_t>(pages.size() % batch_limit != 0), 0);
    const std::set<std::string> existing_set(existing_storage_pages.begin(), existing_storage_pages.end());
    for (size_t index = 0; index < pages.size(); ++index)
        if (existing_set.contains(pages[index])) ++existing_batch_pages[index / batch_limit];
    operation.host_to_storage_schedule = schedule_target_io(normalized_scope(fact),
                                                             "write_host_to_storage",
                                                             submitted_at,
                                                             pages.size(),
                                                             existing_batch_pages);
    operation.host_to_storage_pages = pages;
    operation.host_to_storage_existing_pages = std::move(existing_storage_pages);
    operation.host_to_storage_new_pages = std::move(new_storage_pages);
}

void HiCacheState::materialize_host_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                           const std::string & storage_id) {
    auto * operation = scope.async_ops.storage_operation(storage_id);
    if (operation == nullptr || operation->host_materialized) return;
    if (execution_prefetch_control_ && (!operation->device_completed_at || *operation->device_completed_at > fact.ts))
        throw std::logic_error("Host publication precedes device write completion");
    operation->reserved_host_pages = 0;
    const auto nodes = execution_prefetch_control_ ? backup_nodes(scope.tree, operation->header.pages) : std::vector<HiCacheNodeId>{ node_id };
    for (const auto id : nodes) {
        scope.tree.mark_host_visible(id, false);
        scope.tree.clear_dirty(id);
    }
    sync_capacity(scope, normalized_scope(fact), nodes);
    operation->host_materialized = true;
    if (execution_prefetch_control_) { submit_storage_backup(fact, scope, *operation, fact.ts); }
    scope.async_ops.set_storage_state(storage_id,
                                      HiCacheOperationState::Ready,
                                      execution_prefetch_control_ ? fact.ts : operation->device_to_host_schedule.ready_ts);
    (void)pages;
}

void HiCacheState::complete_storage_backup(const HiCacheFact & fact, ScopedState & scope, const std::string & storage_id) {
    if (storage_id.empty()) return;
    auto * operation = scope.async_ops.storage_operation(storage_id);
    if (operation == nullptr || operation->storage_committed) return;
    if (!operation->host_materialized)
        materialize_host_backup(fact, scope, operation->node_id, operation->header.pages, storage_id);
    const auto nodes = execution_prefetch_control_ ? backup_nodes(scope.tree, operation->header.pages) : std::vector<HiCacheNodeId>{ operation->node_id };
    for (const auto id : nodes) scope.tree.mark_host_visible(id, true);
    scope.storage.mark_readable_pages(normalized_scope(fact), operation->header.pages);
    operation->storage_committed = true;
    scope.async_ops.set_storage_state(storage_id, HiCacheOperationState::Committed, operation->host_to_storage_schedule.ready_ts);
    if (execution_prefetch_control_) {
        sync_capacity(scope, normalized_scope(fact), nodes);
        // Readable backend data does not release the host pin. The scheduler
        // consumes the acknowledged prefix only after its cross-rank MIN.
        scope.backup_ack_queue.push_back(storage_id);
        return;
    }
    const auto ref = scope.refs.release_owner(scope.tree, operation->header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref);
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
    if (!reserve_host_backup_capacity(fact, scope, host_allocation_pages)) return false;
    const bool device_to_host_required = node->residency.device_present && (node->residency.device_dirty || !node->residency.host_visible);
    const auto device_to_host_pages = device_to_host_required ? pages : std::vector<std::string>{};
    const auto storage_id = storage_readable ? begin_storage_backup(fact, scope, node_id, pages, device_to_host_pages) : std::string{};
    if (storage_readable && policy_.write_count_enabled()) {
        hold_write_through_backup_ref(fact, scope, node_id, pages, storage_id);
    }
    else if (storage_readable && execution_prefetch_control_) {
        scope.async_ops.set_storage_capacity_gate_pages(storage_id, pages);
        scope.pending_write_through_backups.push_back({ .storage_operation_id = storage_id, .pages = pages });
    }
    else if (storage_readable) {
        materialize_host_backup(fact, scope, node_id, pages, storage_id);
    }
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
    const auto ref = scope.refs.acquire_lock(scope.tree, owner, chain);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref);
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
void HiCacheState::apply_write_count_policy(const HiCacheFact & fact, ScopedState & scope, const HiCacheInsertResult & insert) {
    if (!policy_.write_count_enabled()) { return; }
    if (fact.lifecycle_kind == "unfinished" && fact.chunked.value_or(false)) return;
    const auto threshold = policy_.write_through_threshold();
    if (threshold == 0) { return; }

    for (const auto node_id : insert.hit_count_nodes) {
        apply_write_count_to_node(fact,
                                  scope,
                                  WriteCountRequest{
                                      .node_id = node_id,
                                      .threshold = threshold,
                                  });
    }
}


} // namespace markov::trace_graph::modules::hicache::model
