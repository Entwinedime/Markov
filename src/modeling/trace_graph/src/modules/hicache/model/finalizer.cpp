/**
 * @file
 * @brief End-of-trace convergence for target-derived HiCache lifecycles.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <ranges>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

/**
 * @brief Settles target-derived pending lifecycles at the trace boundary.
 *
 * Finalization does not invent source timing. It settles target-required write-through payload
 * shape behind a target-only boundary, releases temporary locks, cancels active prefetches
 * without applying them, and drains terminal host reservations that had no later cache-extend
 * boundary.
 */
void HiCacheState::finalize() {
    for (const auto & [name, scope] : scopes_)
        if (scope.lifecycle_return) throw std::logic_error("Cannot finalize an unfinished lifecycle return");
    if (execution_prefetch_control_) return; // End of trace cannot complete I/O or consume a scheduler ACK.
    HiCacheFact fact;
    fact.event_name = "hicache_finalize";
    for (auto & [scope_name, scope] : scopes_) {
        fact.cache_scope = scope_name;
        const auto boundary_epoch = scope.clock.next_boundary_epoch();
        fact.role = "write_through_backup_finalize";
        drain_write_through_backup_refs(fact, scope);
        // Background H2S work remains part of the predicted resource graph even when
        // its acknowledgement falls outside the measured business window.  Force only
        // lifecycle convergence here; no source consumer or E2E endpoint is invented.
        advance_storage_backups(fact, scope, true);
        fact.role = "prefetch_finalize";
        for (auto & op : scope.async_ops.prefetch_ops() | std::views::values) {
            if (op.prefetch_state != HiCachePrefetchState::Pending && op.prefetch_state != HiCachePrefetchState::Ready) continue;
            op.header.boundary_epoch = boundary_epoch;
            op.header.boundary_ts = fact.ts;
            scope.async_ops.set_prefetch_state_by_id(op.header.operation_id, HiCachePrefetchState::Suppressed, HiCacheOperationState::Cancelled, fact.ts);
            op.reserved_host_pages = 0;
            const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
            sync_capacity_for_ref(scope, scope_name, ref);
            sync_capacity(scope, scope_name, {});
        }
        for (auto & op : scope.async_ops.prefetch_ops() | std::views::values) {
            if (op.reserved_host_pages == 0) continue;
            op.reserved_host_pages = 0;
            sync_capacity(scope, scope_name, {});
        }
    }
}
} // namespace markov::trace_graph::modules::hicache::model
