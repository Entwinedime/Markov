/**
 * @file
 * @brief Models the target-derived HiCache storage-prefetch lifecycle.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

HiCacheIoSchedule HiCacheState::schedule_target_io(ScopedState & scope, TargetIoLane lane, uint64_t eligibility_ts, uint64_t page_count) const {
    HiCacheIoSchedule schedule;
    uint64_t bandwidth = 0;
    uint64_t * lane_available_ts = nullptr;
    switch (lane) {
    case TargetIoLane::HostToDevice:
        schedule.resource_lane = "host_to_device_lane";
        bandwidth = config_.io_planning.device_host_bandwidth_bytes_per_sec;
        lane_available_ts = &scope.host_to_device_lane_available_ts;
        break;
    case TargetIoLane::DeviceToHost:
        schedule.resource_lane = "device_to_host_lane";
        bandwidth = config_.io_planning.device_host_bandwidth_bytes_per_sec;
        lane_available_ts = &scope.device_to_host_lane_available_ts;
        break;
    case TargetIoLane::HostStorage:
        schedule.resource_lane = "host_storage_lane";
        bandwidth = config_.io_planning.host_storage_bandwidth_bytes_per_sec;
        lane_available_ts = &scope.host_storage_lane_available_ts;
        break;
    }
    if (page_count == 0 || config_.kv_bytes_per_page == 0 || bandwidth == 0 || lane_available_ts == nullptr) return schedule;

    schedule.effective_byte_count =
        core::checked_multiply_u64(page_count, config_.kv_bytes_per_page, "HiCache target I/O byte projection exceeds uint64 range");
    const auto duration = core::ceil_multiply_divide_u64(schedule.effective_byte_count, 1'000'000, bandwidth);
    if (!duration) throw std::overflow_error("HiCache target I/O duration exceeds uint64 range");
    schedule.duration_us = *duration;
    schedule.start_ts = std::max(eligibility_ts, *lane_available_ts);
    schedule.ready_ts = core::checked_add_u64(schedule.start_ts, schedule.duration_us, "HiCache target I/O ready timestamp exceeds uint64 range");
    schedule.available = true;
    *lane_available_ts = schedule.ready_ts;
    return schedule;
}

void HiCacheState::advance_ready_prefetches(const HiCacheFact & fact) {
    struct ReadyPrefetch {
        const std::string * cache_scope = nullptr;
        ScopedState * scope = nullptr;
        HiCachePrefetchOperation * operation = nullptr;
        const HiCacheFact * source_boundary = nullptr;
        PrefetchProgressEstimate progress;
    };

    std::vector<ReadyPrefetch> ready;
    for (auto & [cache_scope, scope] : scopes_) {
        for (auto & op : scope.async_ops.prefetch_ops() | std::views::values) {
            if (op.prefetch_state != HiCachePrefetchState::Pending) continue;
            const auto * source_boundary = prefetch_control_boundary_for_operation(op);
            if (source_boundary == nullptr) continue;
            auto progress = estimate_prefetch_progress(op, *source_boundary);
            if (!progress.storage_hit_sufficient || !progress.boundary_resolved || progress.target_boundary_ts > fact.ts) continue;
            ready.push_back(ReadyPrefetch{
                .cache_scope = &cache_scope,
                .scope = &scope,
                .operation = &op,
                .source_boundary = source_boundary,
                .progress = std::move(progress),
            });
        }
    }
    std::ranges::sort(ready, [](const auto & left, const auto & right) {
        if (left.progress.target_boundary_ts != right.progress.target_boundary_ts) return left.progress.target_boundary_ts < right.progress.target_boundary_ts;
        if (*left.cache_scope != *right.cache_scope) return *left.cache_scope < *right.cache_scope;
        return left.operation->header.operation_id < right.operation->header.operation_id;
    });
    for (auto & candidate : ready) {
        auto & op = *candidate.operation;
        bind_prefetch_consumer_boundary(*candidate.source_boundary, *candidate.scope, op, op.header.request_key);
        op.header.boundary_ts = candidate.progress.target_boundary_ts;
        op.header.boundary_epoch = op.header.consumer_epoch;
        op.completed_pages = std::move(candidate.progress.completed_pages);
        op.completed_byte_count = candidate.progress.completed_byte_count;
        op.policy_stop_ts = candidate.progress.policy_stop_ts;
        op.target_boundary_ts = candidate.progress.target_boundary_ts;
        op.timeout_deadline_ts = candidate.progress.timeout_deadline_ts;
        op.timed_out = candidate.progress.timed_out;
        op.visibility_dependency_required = candidate.progress.visibility_dependency_required;
        apply_prefetch_ready(*candidate.source_boundary, *candidate.scope, op);
    }
}

void HiCacheState::bind_prefetch_consumer_boundary(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op,
                                                   const std::string & request_key) {
    if (op.header.consumer_epoch != 0) return;
    const auto consumer_epoch =
        scope.clock.record_fact_boundary(normalized_scope(fact), request_key, "prefetch_cache_extend_consumer", fact.source_event_index, fact.ts);
    op.header.consumer_epoch = consumer_epoch;
    op.header.consumer_ts = fact.ts;
    op.header.consumer_source_node_id = fact.source_node_id;
    op.header.consumer_execution_anchor_node_id = fact.execution_anchor_node_id;
    op.header.consumer_source_event_index = fact.source_event_index;
    op.header.consumer_source_fact_role = fact.role;
    op.header.consumer_source_available = true;
}

/**
 * @brief Estimates host-prefix transfer completed by the current target boundary.
 *
 * The input contract exposes a prefetch candidate and later cache-extend boundary.
 * Payload progress is projected from the target lane schedule and calibrated bandwidth;
 * only complete contiguous pages become host-visible state.
 */
HiCacheState::PrefetchIoProgressEstimate HiCacheState::estimate_prefetch_io_progress(const HiCachePrefetchOperation & op, uint64_t boundary_ts) const {
    PrefetchIoProgressEstimate estimate;
    if (!op.io_schedule.available || boundary_ts <= op.io_schedule.start_ts) {
        return estimate;
    }
    if (boundary_ts >= op.io_schedule.ready_ts) {
        estimate.completed_pages = op.hit_pages;
        estimate.completed_byte_count = op.io_schedule.effective_byte_count;
        return estimate;
    }

    const auto elapsed_us = core::checked_subtract_u64(boundary_ts, op.io_schedule.start_ts, "HiCache prefetch progress boundary precedes I/O start");
    const auto completed_bytes =
        core::floor_multiply_divide_u64(elapsed_us, config_.io_planning.host_storage_bandwidth_bytes_per_sec, 1'000'000);
    if (!completed_bytes || config_.kv_bytes_per_page == 0) {
        return estimate;
    }
    estimate.completed_byte_count = std::min(*completed_bytes, op.io_schedule.effective_byte_count);
    const auto completed_page_count = std::min<uint64_t>(static_cast<uint64_t>(op.hit_pages.size()), estimate.completed_byte_count / config_.kv_bytes_per_page);
    estimate.completed_pages = prefix_to(op.hit_pages, static_cast<size_t>(completed_page_count));
    return estimate;
}

/**
 * @brief Resolves one target prefetch stop boundary from the source control skeleton.
 *
 * Best-effort samples progress at the source cache-extend boundary. Wait-complete
 * stops at target I/O completion. Timeout stops at the earlier of target I/O completion
 * and the configured deadline. The source boundary remains the earliest scheduler
 * eligibility point; a later policy stop moves the target consumer through a causal gate.
 */
HiCacheState::PrefetchProgressEstimate HiCacheState::estimate_prefetch_progress(const HiCachePrefetchOperation & op,
                                                                                const HiCacheFact & source_boundary) const {
    const auto token_count = core::checked_multiply_u64(static_cast<uint64_t>(op.hit_pages.size()),
                                                        config_.page_size,
                                                        "HiCache prefetch timeout token projection exceeds uint64 range");
    const auto timeout_input = HiCachePrefetchTimeoutInput{
        .enqueue_ts = op.header.enqueue_ts,
        .token_count = token_count,
    };
    auto estimate = PrefetchProgressEstimate{
        .completed_pages = {},
        .source_boundary_ts = source_boundary.ts,
        .policy_stop_ts = source_boundary.ts,
        .target_boundary_ts = source_boundary.ts,
        .storage_hit_sufficient = op.hit_pages.size() >= policy_.prefetch_threshold_pages(),
    };
    if (!estimate.storage_hit_sufficient) {
        return estimate;
    }
    const auto policy = policy_.prefetch_policy();
    if (policy == "best_effort") {
        estimate.boundary_resolved = true;
        return estimate;
    }
    if (!op.io_schedule.available) {
        return estimate;
    }

    if (policy == "wait_complete") {
        estimate.policy_stop_ts = op.io_schedule.ready_ts;
        estimate.target_boundary_ts = std::max(estimate.source_boundary_ts, estimate.policy_stop_ts);
        estimate.boundary_resolved = true;
        estimate.io_completed = true;
        estimate.visibility_dependency_required = estimate.policy_stop_ts > estimate.source_boundary_ts;
        auto io_progress = estimate_prefetch_io_progress(op, estimate.policy_stop_ts);
        estimate.completed_pages = std::move(io_progress.completed_pages);
        estimate.completed_byte_count = io_progress.completed_byte_count;
        return estimate;
    }
    if (policy == "timeout") {
        const auto timeout_deadline = policy_.prefetch_timeout_deadline_ts(timeout_input);
        if (!timeout_deadline) {
            return estimate;
        }
        estimate.timed_out = *timeout_deadline < op.io_schedule.ready_ts;
        estimate.io_completed = !estimate.timed_out;
        estimate.timeout_deadline_ts = *timeout_deadline;
        estimate.policy_stop_ts = std::min(*timeout_deadline, op.io_schedule.ready_ts);
        estimate.target_boundary_ts = std::max(estimate.source_boundary_ts, estimate.policy_stop_ts);
        estimate.visibility_dependency_required = estimate.policy_stop_ts > estimate.source_boundary_ts;
        estimate.boundary_resolved = true;
        auto io_progress = estimate_prefetch_io_progress(op, estimate.policy_stop_ts);
        estimate.completed_pages = std::move(io_progress.completed_pages);
        estimate.completed_byte_count = io_progress.completed_byte_count;
        return estimate;
    }
    throw std::logic_error("Resolved HiCache prefetch policy is not executable: " + policy);
}

void HiCacheState::suppress_prior_prefetch(const HiCacheFact & fact, ScopedState & scope, const std::string & request_key) {
    auto * prior = scope.async_ops.prefetch_for_request(request_key);
    if (prior == nullptr || !prefetch_active(*prior)) return;
    scope.async_ops.set_prefetch_state_by_id(prior->header.operation_id,
                                             HiCachePrefetchState::Suppressed,
                                             HiCacheOperationState::Cancelled,
                                             "superseded",
                                             fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, prior->header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_supersede_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_supersede_reservation");
}

/**
 * @brief Attempts to enqueue one prefetch under target policy.
 *
 * The decision consumes only the candidate path, target memory/storage prefixes,
 * host capacity, and target rate limit. Source-run success never enters target state.
 */
void HiCacheState::apply_prefetch_candidate_anchor(const HiCacheFact & fact) {
    const auto resolution = token_directory_.resolve_prefetch_candidate_path(fact, pager_.page_size_for_fact(fact));
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    const auto request_key = scoped_request_key(fact);
    if (!request_key.empty()) scope.requests[request_key].prefetch_candidate_seen = true;
    scope.storage.observe_path(page_path);
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "prefetch_lookup_touch");
    /**
     * @brief Defines the memory prefix from device and host residency only.
     *
     * Storage readability is not directly reusable request memory; it contributes
     * only to the later prefetch-I/O candidate.
     */
    const auto memory_prefix = scope.tree.contiguous_prefix(pages, true, true, false);
    auto planned_pages = suffix_from(pages, memory_prefix.size());
    auto planned_projected_pages = suffix_from(page_path.pages, memory_prefix.size());
    if (planned_pages.empty()) {
        return;
    }

    const auto requested_pages = static_cast<uint64_t>(planned_pages.size());
    if (requested_pages < policy_.prefetch_threshold_pages()) {
        return;
    }
    const auto active_requested_pages = scope.async_ops.active_requested_pages(normalized_scope(fact));
    if (policy_.prefetch_rate_limited(active_requested_pages)) {
        return;
    }

    if (request_key.empty()) return;
    const auto prefetch_id = scope.clock.next_operation_id("prefetch");
    const auto enqueue_epoch = scope.clock.next_enqueue_epoch();
    auto owner = request_key + ":" + prefetch_id;
    const auto anchor_nodes = lookup.deepest_host_node == 0 ? std::vector<HiCacheNodeId>{} : std::vector<HiCacheNodeId>{ lookup.deepest_host_node };
    if (!anchor_nodes.empty()) {
        const auto ref = scope.refs.acquire_host(scope.tree, owner, "prefetch", request_key, prefetch_id, anchor_nodes);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_anchor_ref_acquire");
    }

    const auto allocation = request_host_allocation(fact,
                                                    scope,
                                                    HostAllocationRequest{
                                                        .requested_pages = requested_pages,
                                                        .minimum_pages = policy_.prefetch_threshold_pages(),
                                                        .allow_truncate = true,
                                                        .reason = "prefetch",
                                                    });
    if (!allocation.accepted) {
        const auto ref = scope.refs.release_owner(scope.tree, owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_anchor_ref_release_no_capacity");
        return;
    }
    auto reservable = allocation.accepted_pages;
    if (allocation.truncated) {
        planned_pages.resize(static_cast<size_t>(reservable));
        planned_projected_pages.resize(static_cast<size_t>(reservable));
    }
    const auto effective_requested_pages = static_cast<uint64_t>(planned_pages.size());

    /**
     * @brief Treats `hit_pages` as the contiguous readable L3 prefix.
     *
     * This prefix determines whether prefetch is worthwhile and caps the host-visible
     * materialization after termination.
     */
    const auto hit_pages = scope.storage.contiguous_readable_prefix(planned_projected_pages);
    suppress_prior_prefetch(fact, scope, request_key);
    const bool payload_transfer_issued = policy_.prefetch_policy() != "best_effort" && hit_pages.size() >= policy_.prefetch_threshold_pages();
    const auto io_schedule =
        payload_transfer_issued ? schedule_target_io(scope, TargetIoLane::HostStorage, fact.ts, static_cast<uint64_t>(hit_pages.size())) : HiCacheIoSchedule{};

    uint64_t storage_reuse_distance_sum_bytes = 0;
    uint64_t storage_reuse_distance_max_bytes = 0;
    uint64_t storage_reuse_distance_known_pages = 0;
    uint64_t storage_reuse_distance_unknown_pages = 0;
    for (const auto & page : hit_pages) {
        const auto last_access = scope.storage_page_last_access_end_byte.find(page);
        if (last_access == scope.storage_page_last_access_end_byte.end()) {
            storage_reuse_distance_unknown_pages =
                core::checked_increment_u64(storage_reuse_distance_unknown_pages, "HiCache Prefetch unknown storage-reuse page count exceeds uint64 range");
            continue;
        }
        const auto distance = core::checked_subtract_u64(scope.storage_access_bytes_completed,
                                                         last_access->second,
                                                         "HiCache Prefetch storage-reuse position exceeds cumulative accesses");
        storage_reuse_distance_sum_bytes =
            core::checked_add_u64(storage_reuse_distance_sum_bytes, distance, "HiCache Prefetch storage-reuse distance sum exceeds uint64 range");
        storage_reuse_distance_max_bytes = std::max(storage_reuse_distance_max_bytes, distance);
        storage_reuse_distance_known_pages =
            core::checked_increment_u64(storage_reuse_distance_known_pages, "HiCache Prefetch known storage-reuse page count exceeds uint64 range");
    }

    HiCachePrefetchOperation op{
        .header =
            make_operation_header(HiCacheOperationKind::Prefetch, prefetch_id, fact, normalized_scope(fact), request_key, owner, planned_pages, enqueue_epoch),
        .host_insert_pages = prefix_to(pages, memory_prefix.size() + hit_pages.size()),
        .host_visible_offset_pages = static_cast<uint64_t>(memory_prefix.size()),
        .planned_pages = planned_pages,
        .hit_pages = hit_pages,
        .completed_pages = {},
        .io_schedule = io_schedule,
        .payload_transfer_issued = payload_transfer_issued,
        .requested_host_pages = effective_requested_pages,
        .reserved_host_pages = reservable,
        .host_capacity_pages_at_enqueue = allocation.capacity_pages,
        .host_occupied_pages_at_enqueue = allocation.occupied_pages,
        .host_reserved_pages_at_enqueue = allocation.reserved_pages,
        .active_requested_pages_at_enqueue = active_requested_pages,
        .storage_reuse_distance_sum_bytes_at_enqueue = storage_reuse_distance_sum_bytes,
        .storage_reuse_distance_max_bytes_at_enqueue = storage_reuse_distance_max_bytes,
        .storage_reuse_distance_known_pages_at_enqueue = storage_reuse_distance_known_pages,
        .storage_reuse_distance_unknown_pages_at_enqueue = storage_reuse_distance_unknown_pages,
        .prefetch_state = HiCachePrefetchState::Pending,
    };
    scope.async_ops.insert_prefetch(std::move(op));
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_reservation");
}

/**
 * @brief Materializes a completed prefetch prefix into the host radix.
 *
 * `completed_pages` may be empty when a stop boundary is reached before the current
 * approximation observes completed I/O. Lifecycle still advances, while outstanding
 * reservation remains pending for a later drain.
 */
void HiCacheState::apply_prefetch_ready(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op) {
    const auto completion_ts = op.target_boundary_ts != 0 ? op.target_boundary_ts : op.io_schedule.available ? op.io_schedule.ready_ts : fact.ts;
    const bool timeout_incomplete = op.timed_out && op.completed_byte_count < op.io_schedule.effective_byte_count;
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             timeout_incomplete ? HiCachePrefetchState::Late : HiCachePrefetchState::Ready,
                                             timeout_incomplete ? HiCacheOperationState::Completed : HiCacheOperationState::Ready,
                                             timeout_incomplete ? "prefetch_timeout_incomplete" : "completed_prefetch_ready",
                                             completion_ts);

    if (op.host_visible_offset_pages > op.host_insert_pages.size()) throw std::logic_error("HiCache prefetch host-visible offset exceeds its insertion path");
    const auto completed_page_count = static_cast<uint64_t>(op.completed_pages.size());
    const auto host_visible_end =
        core::checked_add_u64(op.host_visible_offset_pages, completed_page_count, "HiCache prefetch host-visible prefix exceeds uint64 range");
    if (host_visible_end > op.host_insert_pages.size()) throw std::logic_error("HiCache completed prefetch prefix exceeds its insertion path");
    const auto visible_pages = std::set<std::string>(op.completed_pages.begin(), op.completed_pages.end());
    (void)scope.tree.lookup(prefix_to(op.host_insert_pages, static_cast<size_t>(op.host_visible_offset_pages)));
    const auto host_insert_pages = prefix_to(op.host_insert_pages, static_cast<size_t>(host_visible_end));
    auto insert = scope.tree.insert_host_path(host_insert_pages, visible_pages, true);
    sync_capacity_for_insert(scope, normalized_scope(fact), insert, "prefetch_insert_host");
    scope.storage.mark_readable_pages(normalized_scope(fact), op.completed_pages);
    for (const auto & page : op.completed_pages) {
        scope.storage_access_bytes_completed = core::checked_add_u64(scope.storage_access_bytes_completed,
                                                                     config_.kv_bytes_per_page,
                                                                     "HiCache cumulative storage-access bytes exceed uint64 range");
        scope.storage_page_last_access_end_byte[page] = scope.storage_access_bytes_completed;
    }
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             timeout_incomplete ? HiCachePrefetchState::Late : HiCachePrefetchState::Applied,
                                             HiCacheOperationState::Committed,
                                             timeout_incomplete ? "apply_timeout_host_visibility" : "apply_host_visibility",
                                             completion_ts);
    /**
     * @brief Converts the completed prefix immediately into host residency.
     *
     * Uncompleted reservation is retained as pending release for request extend or
     * finalization rather than being cleared at this boundary.
     */
    op.reserved_host_pages =
        core::checked_subtract_u64(op.reserved_host_pages, completed_page_count, "HiCache completed prefetch pages exceed reserved host pages");
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_apply_pending_host_release");
}

/**
 * @brief Cancels a prefetch while retaining target-derived pending host reservation.
 *
 * After revoke or incomplete timeout, SGLang releases reservation through background
 * queues and a later scheduling boundary. `reserved_host_pages` retains that pressure
 * so cancellation does not free host capacity prematurely.
 */
void HiCacheState::cancel_prefetch_pending_release(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op,
                                                   const std::string & transition_kind, HiCachePrefetchState prefetch_state) {
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id, prefetch_state, HiCacheOperationState::Cancelled, transition_kind, fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_cancel_pending_host_release");
}

/**
 * @brief Settles the request's active prefetch before cache-extend side effects.
 *
 * The current contract does not capture source runtime checkpoints. A request's
 * `cache_extend_input` is therefore its target-derived terminal prefetch boundary,
 * where target policy selects apply, revoke, or incomplete timeout.
 */
void HiCacheState::settle_prefetch_before_cache_extend(const HiCacheFact & fact, ScopedState & scope, const std::string & request_key) {
    auto * op = scope.async_ops.prefetch_for_request(request_key);
    if (op == nullptr) {
        return;
    }
    if (!prefetch_active(*op)) {
        if (op->visibility_dependency_required) bind_prefetch_consumer_boundary(fact, scope, *op, request_key);
        return;
    }
    bind_prefetch_consumer_boundary(fact, scope, *op, request_key);
    op->visibility_dependency_required = false;

    auto suppress = [&](const std::string & kind, HiCachePrefetchState state) { cancel_prefetch_pending_release(fact, scope, *op, kind, state); };

    if (op->hit_pages.size() < policy_.prefetch_threshold_pages()) {
        op->header.boundary_ts = fact.ts;
        op->header.boundary_epoch = op->header.consumer_epoch;
        op->policy_stop_ts = fact.ts;
        op->target_boundary_ts = fact.ts;
        op->timeout_deadline_ts = 0;
        suppress("prefetch_revoked", HiCachePrefetchState::Revoked);
        drain_prefetch_pending_release(
            fact,
            scope,
            request_key,
            PrefetchReleaseReasons{
                .capacity = "prefetch_storage_control_pre_cache_extend_host_release",
                .policy = "Below-threshold target prefetch is revoked and its host reservation is drained before cache extend allocation",
            });
        return;
    }

    const auto policy = policy_.prefetch_policy();
    auto progress = estimate_prefetch_progress(*op, fact);
    if (!progress.boundary_resolved) {
        return;
    }

    op->header.boundary_ts = progress.target_boundary_ts;
    op->header.boundary_epoch = op->header.consumer_epoch;
    op->completed_pages = std::move(progress.completed_pages);
    op->completed_byte_count = progress.completed_byte_count;
    op->policy_stop_ts = progress.policy_stop_ts;
    op->target_boundary_ts = progress.target_boundary_ts;
    op->timeout_deadline_ts = progress.timeout_deadline_ts;
    op->timed_out = progress.timed_out;
    op->visibility_dependency_required = progress.visibility_dependency_required;
    apply_prefetch_ready(fact, scope, *op);
}

/**
 * @brief Releases terminal-prefetch host reservation at a scheduler/extend boundary.
 */
void HiCacheState::drain_prefetch_pending_release(const HiCacheFact & fact, ScopedState & scope, const std::string & request_key,
                                                  const PrefetchReleaseReasons & reasons) {
    const auto released_pages = scope.async_ops.release_prefetch_pending_host_pages_for_request(request_key);
    if (released_pages == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, std::string(reasons.capacity));
    (void)reasons;
}


} // namespace markov::trace_graph::modules::hicache::model
