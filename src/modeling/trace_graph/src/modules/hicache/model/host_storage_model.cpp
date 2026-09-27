/**
 * @file
 * @brief Models HiCache host capacity, storage backup, and device eviction.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <string>
#include <set>
#include <tuple>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

bool HiCacheState::commit_device_eviction_writeback(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id,
                                                    const std::vector<std::string> & pages) {
    const auto writeback_id = scope.clock.next_operation_id("writeback");
    const auto writeback_owner = scoped_request_key(fact) + ":writeback:" + writeback_id;
    // Storage owns the transfer lifecycle; the pending victim owns only eviction's lock and release.
    auto ref = scope.refs.acquire_lock(scope.tree, writeback_owner, std::vector<HiCacheNodeId>{ node_id });
    sync_capacity_for_ref(scope, normalized_scope(fact), ref);

    const auto committed = commit_host_backup(fact, scope, node_id, true);
    if (committed && execution_prefetch_control_) {
        if (!scope.allocation) throw std::logic_error("Device writeback requires a resumable allocation");
        scope.allocation->victims.push_back({ node_id, pages, writeback_owner });
        return true;
    }
    ref = scope.refs.release_owner(scope.tree, writeback_owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    return committed;
}

uint64_t HiCacheState::release_device_residency(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages) {
    const auto released_pages = static_cast<uint64_t>(pages.size());
    const auto * node = scope.tree.node(node_id);
    const bool has_backup = node != nullptr && has_host_backup(*node);
    std::vector<HiCacheNodeId> affected_nodes{ node_id };
    if (has_backup) { scope.tree.demote_device_to_host(node_id, false); }
    else {
        const auto removal = scope.tree.evict_unbacked_device_leaf(node_id);
        if (!removal.evicted) {
            return 0;
        }
        affected_nodes = removal.affected_nodes;
    }
    const auto allocator_released_pages = scope.device_allocator.release(released_pages);
    sync_capacity(scope, normalized_scope(fact), affected_nodes);
    (void)allocator_released_pages;
    return released_pages;
}

/**
 * @brief Evicts one device node, committing dirty write-back data first.
 *
 * Device eviction updates radix residency, allocator release state, and the capacity
 * index. Dirty write-back waits for the modeled D2H acknowledgement; its H2S storage
 * backup remains an independent background lifecycle.
 */
uint64_t HiCacheState::evict_device_node(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id) {
    auto * node = scope.tree.mutable_node(node_id);
    if (node == nullptr || !node->residency.device_present) return 0;

    const auto pages = node->pages;
    const bool needs_writeback = policy_.write_back_enabled() && node->residency.device_dirty;
    if (needs_writeback && !commit_device_eviction_writeback(fact, scope, node_id, pages)) return 0;
    if (needs_writeback && execution_prefetch_control_) return pages.size(); // Selected, not yet released.
    return release_device_residency(fact, scope, node_id, pages);
}

bool HiCacheState::allocation_pending(const HiCacheFact & fact) const {
    const auto scope = scopes_.find(normalized_scope(fact));
    return scope != scopes_.end() && scope->second.allocation && scope->second.allocation->fact_id == fact.source_node_id;
}

const HiCacheEvictionWork * HiCacheState::eviction_work(const HiCacheFact & fact) const {
    const auto scope = scopes_.find(normalized_scope(fact));
    if (scope == scopes_.end()) return nullptr;
    const auto work = scope->second.eviction_work.find(fact.source_node_id);
    return work == scope->second.eviction_work.end() ? nullptr : &work->second;
}

void HiCacheState::allocate_after_capacity(const HiCacheFact & fact, ScopedState & scope, uint64_t requested_pages,
                                         std::function<void(const HiCacheFact &)> continuation) {
    if (!execution_prefetch_control_) {
        enforce_device_capacity(fact, scope, requested_pages);
        continuation(fact);
        return;
    }
    if (scope.allocation) throw std::logic_error("Another allocation is still waiting for writeback");
    ensure_device_allocator(scope);
    if (!scope.device_allocator.should_evict(requested_pages)) {
        continuation(fact);
        return;
    }
    scope.allocation = PendingAllocation{ .fact_id = fact.source_node_id, .continuation = std::move(continuation) };
    auto & work = scope.eviction_work[fact.source_node_id];
    work = HiCacheEvictionWork{ .requested_pages = requested_pages };
    using Candidate = std::tuple<int64_t, uint64_t, HiCacheNodeId>;
    std::set<Candidate> candidates;
    std::set<HiCacheNodeId> selected, queued;
    const auto enqueue = [&](HiCacheNodeId id) {
        const auto * node = scope.tree.node(id);
        if (id && node && queued.insert(id).second) candidates.emplace(node->priority, node->last_access_order, id);
    };
    for (const auto id : scope.capacity.device_victims()) enqueue(id);
    uint64_t planned_pages = 0;
    while (planned_pages < requested_pages && !candidates.empty()) {
        const auto heap_size = candidates.size();
        const auto id = std::get<2>(*candidates.begin());
        candidates.erase(candidates.begin());
        const auto * node = scope.tree.node(id);
        if (!node || node->refs.lock_ref_total || !node->residency.device_present) {
            work.rejected_candidates.push_back({id, work.victims.size(),
                !node ? "missing_node" : node->refs.lock_ref_total ? "locked_node" : "not_device_resident", heap_size});
            continue;
        }
        const auto parent_id = node->parent;
        const bool release_after_write = policy_.write_back_enabled() && node->residency.device_dirty;
        const bool backed_up = release_after_write || has_host_backup(*node);
        const auto pages = evict_device_node(fact, scope, id);
        if (pages) {
            work.victims.push_back({ id, pages, release_after_write, backed_up });
            selected.insert(id);
            planned_pages = core::checked_add_u64(planned_pages, pages, "Eviction budget overflow");
        }
        else {
            work.rejected_candidates.push_back({id, work.victims.size(),
                release_after_write ? "host_backup_rejected" : "device_release_rejected", heap_size});
        }
        // A failed host reservation does not prevent trying another victim.
        // Selected children still occupy GPU pages until the final wait ends.
        const auto * parent = scope.tree.node(parent_id);
        if (parent && std::ranges::all_of(parent->children, [&](const auto & child) {
                const auto * value = scope.tree.node(child.second);
                return !value || !value->residency.device_present || selected.contains(child.second);
            }))
            enqueue(parent_id);
    }
    if (policy_.write_back_enabled() && !scope.pending_write_through_backups.empty()) return;
    auto complete = std::move(scope.allocation->continuation);
    scope.allocation.reset();
    complete(fact);
}

void HiCacheState::confirm_allocation_writes(const HiCacheFact & fact) {
    auto & scope = scope_state(fact);
    if (!allocation_pending(fact)) throw std::logic_error("Allocation continuation does not match its entry");
    const auto count = scope.pending_write_through_backups.size();
    if (write_completion_count(fact) != count) throw std::logic_error("Allocation cannot resume before all queued writes finish");
    if (count) acknowledge_writes(fact, count);
}

void HiCacheState::resume_allocation(const HiCacheFact & fact) {
    confirm_allocation_writes(fact);
    auto & scope = scope_state(fact);
    auto pending = std::move(*scope.allocation);
    scope.allocation.reset();
    for (const auto & victim : pending.victims) {
        (void)release_device_residency(fact, scope, victim.node, victim.pages);
        const auto ref = scope.refs.release_owner(scope.tree, victim.owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    }
    // ACK publishes host data, but capacity is consumed only after release.
    // Keep the independent H2S submission at its earlier ACK time.
    const auto consumer_epoch = scope.clock.next_boundary_epoch();
    for (const auto & [id, operation] : scope.async_ops.storage_ops())
        if (operation.header.source_node_id == fact.source_node_id && !operation.capacity_gate_pages.empty() && !operation.header.consumer_epoch)
            scope.async_ops.set_storage_consumer_boundary(id, consumer_epoch, fact.ts, fact.source_node_id, fact.execution_anchor_node_id,
                                                        fact.source_event_index, fact.role, true);
    pending.continuation(fact);
}

/**
 * @brief Attempts to evict one host leaf under SGLang rules.
 *
 * Host cleanup skips leaves with a positive host-reference count. Successful eviction
 * removes the host leaf or subtree rather than merely clearing `node.host_value`.
 */
uint64_t HiCacheState::evict_host_node(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id) {
    const auto * node = scope.tree.node(node_id);
    const auto pages = node == nullptr ? std::vector<std::string>{} : node->pages;
    if (node != nullptr && node->refs.host_ref_total > 0) {
        sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id });
        return 0;
    }

    const auto result = scope.tree.evict_host_leaf(node_id);
    sync_capacity(scope, normalized_scope(fact), result.affected_nodes.empty() ? std::vector<HiCacheNodeId>{ node_id } : result.affected_nodes);
    if (!result.evicted) return 0;
    return static_cast<uint64_t>(pages.size());
}

/**
 * @brief Enforces device cleanup through the SGLang allocator availability gate.
 *
 * Cleanup is not driven directly by final occupancy. The model reconstructs
 * `available_size` before allocation and passes the full request budget to radix
 * eviction only when availability is insufficient.
 */
DeviceCapacityEnforcementResult HiCacheState::enforce_device_capacity(const HiCacheFact & fact, ScopedState & scope, uint64_t requested_pages) {
    DeviceCapacityEnforcementResult result;
    ensure_device_allocator(scope);
    const auto capacity = scope.device_allocator.capacity_pages;
    if (capacity == 0 || requested_pages == 0) return result;
    sync_capacity(scope, normalized_scope(fact), {});
    if (!scope.device_allocator.should_evict(requested_pages)) { return result; }
    auto target = requested_pages;
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {});
        const auto victim = scope.capacity.first_device_victim();
        if (!victim) break;
        const auto * victim_node = scope.tree.node(*victim);
        const bool dirty_writeback = victim_node != nullptr && policy_.write_back_enabled() && victim_node->residency.device_dirty;
        const auto released_pages = evict_device_node(fact, scope, *victim);
        if (released_pages > 0) {
            result.evicted_node_count = core::checked_add_u64(result.evicted_node_count, 1, "device cleanup evicted-node count exceeds uint64 range");
            result.evicted_page_count =
                core::checked_add_u64(result.evicted_page_count, released_pages, "device cleanup evicted-page count exceeds uint64 range");
            if (dirty_writeback) {
                result.dirty_evicted_node_count =
                    core::checked_add_u64(result.dirty_evicted_node_count, 1, "device cleanup dirty-node count exceeds uint64 range");
                result.dirty_evicted_page_count =
                    core::checked_add_u64(result.dirty_evicted_page_count, released_pages, "device cleanup dirty-page count exceeds uint64 range");
            }
        }
        if (released_pages == 0 || released_pages >= target) break;
        target -= released_pages;
    }
    return result;
}

/**
 * @brief Enforces host cleanup at allocation or capacity-audit boundaries.
 *
 * A positive request drives cleanup with the allocation budget; a zero request removes
 * only existing excess. Reserved host pages contribute pressure so active and pending
 * prefetches cannot be ignored.
 */
void HiCacheState::enforce_host_capacity(const HiCacheFact & fact, ScopedState & scope, uint64_t requested_pages) {
    const auto capacity = policy_.l2_capacity_pages();
    if (capacity == 0) return;
    sync_capacity(scope, normalized_scope(fact), {});
    const auto snapshot = scope.capacity.snapshot();
    auto target = allocation_cleanup_target(HostCleanupInput{
        .occupied_pages = snapshot.occupied_host_pages,
        .reserved_pages = snapshot.reserved_host_pages,
        .capacity_pages = capacity,
        .requested_pages = requested_pages,
    });
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {});
        const auto victim = scope.capacity.first_host_victim();
        if (!victim) break;
        const auto victim_pages = evict_host_node(fact, scope, *victim);
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
HiCacheState::HostAllocationResult HiCacheState::request_host_allocation(const HiCacheFact & fact, ScopedState & scope, const HostAllocationRequest & request) {
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

    enforce_host_capacity(fact, scope, request.requested_pages);
    sync_capacity(scope, normalized_scope(fact), {});
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
