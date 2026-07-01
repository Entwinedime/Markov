/**
 * @file
 * @brief HiCache state model 的 trace 结束收敛逻辑。
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
 * @brief trace 结束时收敛 target-derived pending lifecycle。
 *
 * finalize 不补 source actual 结果，只把 target-derived pending lifecycle 收敛到一个可验证
 * final state：write-through backup ACK 释放尾部 ordinary lock；active prefetch 不再
 * apply，未被后续 cache extend 消费的 host reservation 在 final 边界释放。
 */
void HiCacheState::finalize(HiCacheSummary & summary, HiCacheTransitionBuffer & transitions) {
    HiCacheFact fact;
    fact.event_name = "hicache_finalize";
    for (auto & [scope_name, scope] : scopes_) {
        fact.cache_scope = scope_name;
        const auto boundary = scope.clock.record_target_finalize_boundary(scope_name, fact.ts);
        fact.role = "write_through_backup_finalize";
        drain_write_through_backup_refs(fact, summary, transitions, scope, "write_through_backup_finalize_boundary");
        fact.role = "prefetch_finalize";
        for (auto & op : scope.async_ops.prefetch_ops() | std::views::values) {
            if (op.prefetch_state != HiCachePrefetchState::Pending && op.prefetch_state != HiCachePrefetchState::Ready) continue;
            op.header.boundary_epoch = boundary.boundary_epoch;
            op.header.boundary_ts = fact.ts;
            const auto before = debug_state_digest();
            if constexpr (debug_records_enabled())
                record_policy_decision(fact,
                                       HiCachePolicyDecisionRecord{
                                           .operation_id = op.header.operation_id,
                                           .policy_area = "prefetch_finalize",
                                           .policy_name = policy_.prefetch_policy(),
                                           .decision = "cancel_pending_prefetch",
                                           .reason = "target finalize cancels active prefetch without terminal apply",
                                           .accepted = false,
                                           .requested_pages = op.requested_host_pages,
                                           .candidate_pages = static_cast<uint64_t>(op.planned_pages.size()),
                                           .hit_pages = static_cast<uint64_t>(op.hit_pages.size()),
                                           .reserved_pages = op.reserved_host_pages,
                                           .threshold_pages = policy_.prefetch_threshold_pages(),
                                           .pages = op.planned_pages,
                                       });
            scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                                     HiCachePrefetchState::Suppressed,
                                                     HiCacheOperationState::Cancelled,
                                                     "target_finalize",
                                                     fact.ts);
            op.reserved_host_pages = 0;
            const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
            sync_capacity_for_ref(scope, scope_name, ref, "prefetch_finalize_ref_release");
            sync_capacity(scope, scope_name, {}, "prefetch_finalize_reservation");
            if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "prefetch_suppressed", "prefetch", op.planned_pages, before);
        }
        for (auto & op : scope.async_ops.prefetch_ops() | std::views::values) {
            if (op.reserved_host_pages == 0) continue;
            const auto released_pages = op.reserved_host_pages;
            if constexpr (debug_records_enabled())
                record_policy_decision(fact,
                                       HiCachePolicyDecisionRecord{
                                           .operation_id = op.header.operation_id,
                                           .policy_area = "prefetch_finalize",
                                           .policy_name = "prefetch_host_release_queue",
                                           .decision = "release_unadmitted_prefetch_pending_host_pages",
                                           .reason = "finalize releases terminal prefetch host reservation that had no later cache extend drain",
                                           .accepted = true,
                                           .requested_pages = op.requested_host_pages,
                                           .candidate_pages = static_cast<uint64_t>(op.planned_pages.size()),
                                           .hit_pages = static_cast<uint64_t>(op.hit_pages.size()),
                                           .reserved_pages = released_pages,
                                           .allocator_released_pages = released_pages,
                                           .threshold_pages = policy_.prefetch_threshold_pages(),
                                           .pages = op.planned_pages,
                                       });
            op.reserved_host_pages = 0;
            sync_capacity(scope, scope_name, {}, "prefetch_finalize_pending_host_release");
        }
    }
}


} // namespace markov::trace_graph::modules::hicache::model
