/**
 * @file
 * @brief Models the target-derived HiCache storage-prefetch lifecycle.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"
#include "markov/trace_graph/modules/hicache/service_model.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

HiCacheIoSchedule HiCacheState::schedule_target_io(const std::string & scope, const std::string & kind, uint64_t eligibility_ts, uint64_t page_count,
                                                   std::span<const uint64_t> existing_batch_pages, std::optional<uint64_t> stop_ts) {
    HiCacheIoSchedule schedule;
    if (page_count == 0) return schedule;
    const bool storage = kind == "prefetch" || kind == "write_host_to_storage";
    const auto batch_limit = storage ? config_.io_cost.storage_batch_pages : page_count;
    if (batch_limit == 0) throw std::logic_error("Storage service requires a positive batch limit");
    const auto batch_count = page_count / batch_limit + static_cast<uint64_t>(page_count % batch_limit != 0);
    if (!existing_batch_pages.empty() && (kind != "write_host_to_storage" || existing_batch_pages.size() != batch_count))
        throw std::logic_error("H2S existing-page work must describe every executed service batch");
    const auto & service = require_hicache_service(config_.io_cost,
                                                   kind,
                                                   config_.kv_bytes_per_page,
                                                   std::min(batch_limit, page_count),
                                                   existing_batch_pages.empty() ? 0 : existing_batch_pages.front());
    schedule.resource_lane = hicache_resource_lane(config_.io_cost, kind, scope);
    auto & lane_available = io_lane_available_ts_[schedule.resource_lane];
    schedule.start_ts = std::max(eligibility_ts, lane_available);
    schedule.ready_ts = schedule.start_ts;
    const bool staged_prefetch = kind == "prefetch";
    uint64_t executed_pages = 0;
    for (uint64_t start = 0; start < page_count;) {
        // The SGLang payload worker enters its first batch even if already
        // terminated; subsequent batches stop after the cancelled call returns.
        if (!staged_prefetch && start > 0 && stop_ts && schedule.ready_ts >= *stop_ts) break;
        const auto count = std::min(batch_limit, page_count - start);
        const auto existing = existing_batch_pages.empty() ? 0 : existing_batch_pages[schedule.batches.size()];
        const auto cost = hicache_service_cost(service, config_.kv_bytes_per_page, count, 1, existing);
        if (!cost) throw std::logic_error("Target I/O service projection is invalid");
        const auto ready = core::checked_add_u64(schedule.ready_ts, cost->duration_us, "Target I/O completion overflow");
        schedule.batches.push_back({ count, schedule.ready_ts, ready });
        bool cancelled = false;
        if (staged_prefetch) {
            const auto staged = hicache_prefetch_batch(service, config_.kv_bytes_per_page, count, schedule.ready_ts);
            if (!staged) throw std::logic_error("Target prefetch stages are invalid");
            const auto execution = execute_prefetch_batch(*staged, stop_ts);
            auto & batch = schedule.batches.back();
            batch.copied_page_count = execution.copied_pages;
            batch.ready_ts = execution.release_us;
            cancelled = execution.cancelled;
            for (size_t page = 0; page < execution.published_pages; ++page) batch.page_ready_ts.push_back(staged->publications[page].at_us);
        }
        schedule.ready_ts = schedule.batches.back().ready_ts;
        executed_pages += count;
        start += count;
        if (cancelled) break;
    }
    schedule.effective_byte_count = core::checked_multiply_u64(executed_pages, config_.kv_bytes_per_page, "Target I/O byte count overflow");
    schedule.duration_us = schedule.ready_ts - schedule.start_ts;
    schedule.available = true;
    lane_available = schedule.ready_ts;
    return schedule;
}

void HiCacheState::advance_ready_prefetches(const HiCacheFact & fact) {
    if (execution_prefetch_control_) return;
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
        bind_prefetch_consumer_boundary(*candidate.source_boundary, *candidate.scope, op);
        op.header.boundary_ts = candidate.progress.target_boundary_ts;
        op.header.boundary_epoch = op.header.consumer_epoch;
        op.completed_pages = std::move(candidate.progress.completed_pages);
        op.completed_byte_count = candidate.progress.completed_byte_count;
        op.policy_stop_ts = candidate.progress.policy_stop_ts;
        op.target_boundary_ts = candidate.progress.target_boundary_ts;
        op.timeout_deadline_ts = candidate.progress.timeout_deadline_ts;
        op.timed_out = candidate.progress.timed_out;
        op.policy_wait_duration_us = candidate.progress.policy_wait_duration_us;
        op.service_consumer_dependency_required = candidate.progress.service_consumer_dependency_required;
        op.visibility_dependency_required = candidate.progress.visibility_dependency_required;
        apply_prefetch_ready(*candidate.source_boundary, *candidate.scope, op);
    }
}

void HiCacheState::bind_prefetch_consumer_boundary(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op) {
    if (op.header.consumer_epoch != 0) return;

    const auto consumer_epoch = scope.clock.next_boundary_epoch();
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
    if (!op.io_schedule.available || boundary_ts <= op.io_schedule.start_ts) { return estimate; }
    uint64_t completed_pages = 0;
    for (const auto & batch : op.io_schedule.batches) {
        if (!batch.copied_page_count) {
            if (batch.ready_ts > boundary_ts) break;
            completed_pages += batch.page_count;
        }
        else {
            const auto ready = std::ranges::upper_bound(batch.page_ready_ts, boundary_ts) - batch.page_ready_ts.begin();
            completed_pages += ready;
            if (static_cast<uint64_t>(ready) != batch.page_count) break;
        }
    }
    estimate.completed_pages = prefix_to(op.service_pages, static_cast<size_t>(completed_pages));
    estimate.completed_byte_count = core::checked_multiply_u64(completed_pages, config_.kv_bytes_per_page, "Prefetch completed bytes overflow");
    return estimate;
}

/**
 * @brief Resolves one target prefetch stop boundary from the source control skeleton.
 *
 * Best-effort samples progress immediately after target enqueue. The later source
 * cache-extend fact is only the reusable DAG/control anchor; carrying its source
 * wait into this check would turn a source wait-complete policy into a target wait.
 * Wait-complete
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
    if (!estimate.storage_hit_sufficient) { return estimate; }
    const auto policy = policy_.prefetch_policy();
    if (policy == "best_effort") {
        estimate.policy_stop_ts = op.header.enqueue_ts;
        estimate.target_boundary_ts = op.header.enqueue_ts;
        estimate.boundary_resolved = true;
        auto progress = estimate_prefetch_io_progress(op, estimate.policy_stop_ts);
        estimate.completed_pages = std::move(progress.completed_pages);
        estimate.completed_byte_count = progress.completed_byte_count;
        estimate.io_completed = estimate.completed_pages.size() == op.hit_pages.size();
        estimate.visibility_dependency_required = !estimate.completed_pages.empty();
        return estimate;
    }
    if (!op.io_schedule.available) { return estimate; }

    if (policy == "wait_complete") {
        estimate.policy_stop_ts = op.io_schedule.ready_ts;
        estimate.target_boundary_ts = std::max(estimate.source_boundary_ts, estimate.policy_stop_ts);
        estimate.boundary_resolved = true;
        estimate.io_completed = true;
        estimate.service_consumer_dependency_required = true;
        estimate.visibility_dependency_required = estimate.io_completed || !estimate.completed_pages.empty();
        auto io_progress = estimate_prefetch_io_progress(op, estimate.policy_stop_ts);
        estimate.completed_pages = std::move(io_progress.completed_pages);
        estimate.completed_byte_count = io_progress.completed_byte_count;
        return estimate;
    }
    if (policy == "timeout") {
        const auto timeout_deadline = policy_.prefetch_timeout_deadline_ts(timeout_input);
        if (!timeout_deadline) { return estimate; }
        estimate.io_completed = op.service_pages.size() == op.hit_pages.size() && op.io_schedule.ready_ts <= *timeout_deadline;
        estimate.timed_out = !estimate.io_completed;
        estimate.timeout_deadline_ts = *timeout_deadline;
        estimate.policy_stop_ts = estimate.io_completed ? op.io_schedule.ready_ts : *timeout_deadline;
        estimate.service_consumer_dependency_required = estimate.io_completed;
        estimate.policy_wait_duration_us =
            estimate.timed_out ? core::checked_subtract_u64(*timeout_deadline, op.header.enqueue_ts, "HiCache prefetch timeout precedes enqueue") : 0;
        estimate.target_boundary_ts = std::max(estimate.source_boundary_ts, estimate.policy_stop_ts);
        estimate.boundary_resolved = true;
        auto io_progress = estimate_prefetch_io_progress(op, estimate.policy_stop_ts);
        estimate.completed_pages = std::move(io_progress.completed_pages);
        estimate.completed_byte_count = io_progress.completed_byte_count;
        estimate.visibility_dependency_required = estimate.io_completed || !estimate.completed_pages.empty();
        return estimate;
    }
    throw std::logic_error("Resolved HiCache prefetch policy is not executable: " + policy);
}

void HiCacheState::suppress_prior_prefetch(const HiCacheFact & fact, ScopedState & scope, const std::string & request_key) {
    auto * prior = scope.async_ops.prefetch_for_request(request_key);
    if (prior == nullptr || !prefetch_active(*prior)) return;
    scope.async_ops.set_prefetch_state_by_id(prior->header.operation_id, HiCachePrefetchState::Suppressed, HiCacheOperationState::Cancelled, fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, prior->header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    sync_capacity(scope, normalized_scope(fact), {});
}

/**
 * @brief Attempts to enqueue one prefetch under target policy.
 *
 * The decision consumes only the candidate path, target memory/storage prefixes,
 * host capacity, and target rate limit. Source-run success never enters target state.
 */
void HiCacheState::apply_prefetch_candidate_anchor(const HiCacheFact & fact) {
    const auto resolution = runtime::resolve_prefetch_candidate_path(fact);
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    const auto request_key = scoped_request_key(fact);
    if (!request_key.empty()) scope.requests[request_key].prefetch_candidate_seen = true;
    scope.storage.observe_path(page_path);
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain);
    /**
     * @brief Defines the memory prefix from device and host residency only.
     *
     * Storage readability is not directly reusable request memory; it contributes
     * only to the later prefetch-I/O candidate.
     */
    const auto memory_prefix = scope.tree.contiguous_prefix(pages, true, true, false);
    auto planned_pages = suffix_from(pages, memory_prefix.size());
    auto planned_projected_pages = suffix_from(page_path.pages, memory_prefix.size());
    if (planned_pages.empty()) { return; }

    const auto requested_pages = static_cast<uint64_t>(planned_pages.size());
    if (requested_pages < policy_.prefetch_threshold_pages()) { return; }
    const auto active_requested_pages = scope.async_ops.active_requested_pages(normalized_scope(fact));
    if (policy_.prefetch_rate_limited(active_requested_pages)) { return; }

    if (request_key.empty()) return;
    const auto prefetch_id = scope.clock.next_operation_id("prefetch");
    const auto enqueue_epoch = scope.clock.next_enqueue_epoch();
    auto owner = request_key + ":" + prefetch_id;
    const auto anchor_nodes = lookup.deepest_host_node == 0 ? std::vector<HiCacheNodeId>{} : std::vector<HiCacheNodeId>{ lookup.deepest_host_node };
    if (!anchor_nodes.empty()) {
        const auto ref = scope.refs.acquire_host(scope.tree, owner, anchor_nodes);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    }

    const auto allocation = request_host_allocation(fact,
                                                    scope,
                                                    HostAllocationRequest{
                                                        .requested_pages = requested_pages,
                                                        .minimum_pages = policy_.prefetch_threshold_pages(),
                                                        .allow_truncate = true,
                                                    });
    if (!allocation.accepted) {
        const auto ref = scope.refs.release_owner(scope.tree, owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
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
    const auto hit_pages = execution_prefetch_control_ ? std::vector<std::string>{} : scope.storage.contiguous_readable_prefix(planned_projected_pages);
    suppress_prior_prefetch(fact, scope, request_key);
    const bool payload_transfer_issued = !execution_prefetch_control_ && hit_pages.size() >= policy_.prefetch_threshold_pages();
    std::optional<uint64_t> stop;
    if (!execution_prefetch_control_ && policy_.prefetch_policy() == "best_effort") {
        if (const auto * boundary = prefetch_control_boundary_for_lookup(fact)) stop = boundary->ts;
    }
    else if (!execution_prefetch_control_ && policy_.prefetch_policy() == "timeout") {
        stop = policy_.prefetch_timeout_deadline_ts(
            { .enqueue_ts = fact.ts, .token_count = core::checked_multiply_u64(hit_pages.size(), config_.page_size, "Prefetch timeout tokens overflow") });
    }
    const auto io_schedule =
        payload_transfer_issued ? schedule_target_io(normalized_scope(fact), "prefetch", fact.ts, hit_pages.size(), {}, stop) : HiCacheIoSchedule{};

    HiCachePrefetchOperation op{
        .header = make_operation_header(prefetch_id, fact, normalized_scope(fact), request_key, owner, planned_pages, enqueue_epoch),
        .host_insert_pages = prefix_to(pages, memory_prefix.size() + hit_pages.size()),
        .host_visible_offset_pages = static_cast<uint64_t>(memory_prefix.size()),
        .planned_pages = planned_pages,
        .hit_pages = hit_pages,
        .service_pages = prefix_to(hit_pages, io_schedule.available ? io_schedule.effective_byte_count / config_.kv_bytes_per_page : 0),
        .completed_pages = {},
        .io_schedule = io_schedule,
        .payload_transfer_issued = payload_transfer_issued,
        .requested_host_pages = effective_requested_pages,
        .reserved_host_pages = reservable,
        .host_reserved_pages_at_enqueue = allocation.reserved_pages,
        .prefetch_state = HiCachePrefetchState::Pending,
    };
    scope.async_ops.insert_prefetch(std::move(op));
    sync_capacity(scope, normalized_scope(fact), {});
}

/**
 * @brief Materializes a completed prefetch prefix into the host radix.
 *
 * `completed_pages` may be empty when a stop boundary is reached before the current
 * approximation observes completed I/O. Lifecycle still advances, while outstanding
 * reservation remains pending for a later drain.
 */
void HiCacheState::apply_prefetch_ready(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op) {
    // Both asynchronous-ready advancement and the request boundary converge here.
    const auto & capacity = scope.capacity.snapshot();
    const auto used = core::checked_add_u64(capacity.occupied_host_pages, capacity.reserved_host_pages, "HiCache terminal host capacity exceeds uint64 range");
    if (config_.l2_capacity_pages > 0 && used <= config_.l2_capacity_pages) op.host_available_pages_at_control = config_.l2_capacity_pages - used;
    const auto completion_ts = op.target_boundary_ts != 0 ? op.target_boundary_ts : op.io_schedule.available ? op.io_schedule.ready_ts : fact.ts;
    const bool timeout_incomplete = op.timed_out && op.completed_pages.size() < op.hit_pages.size();
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             timeout_incomplete ? HiCachePrefetchState::Late : HiCachePrefetchState::Ready,
                                             timeout_incomplete ? HiCacheOperationState::Completed : HiCacheOperationState::Ready,
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
    sync_capacity_for_insert(scope, normalized_scope(fact), insert);
    scope.storage.mark_readable_pages(normalized_scope(fact), op.completed_pages);

    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             timeout_incomplete ? HiCachePrefetchState::Late : HiCachePrefetchState::Applied,
                                             HiCacheOperationState::Committed,
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
    sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    sync_capacity(scope, normalized_scope(fact), {});
}

/**
 * @brief Cancels a prefetch while retaining target-derived pending host reservation.
 *
 * After revoke or incomplete timeout, SGLang releases reservation through background
 * queues and a later scheduling boundary. `reserved_host_pages` retains that pressure
 * so cancellation does not free host capacity prematurely.
 */
void HiCacheState::cancel_prefetch_pending_release(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op,
                                                   HiCachePrefetchState prefetch_state) {
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id, prefetch_state, HiCacheOperationState::Cancelled, fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    sync_capacity(scope, normalized_scope(fact), {});
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
    if (op == nullptr) { return; }
    if (!prefetch_active(*op)) {
        if (op->visibility_dependency_required) bind_prefetch_consumer_boundary(fact, scope, *op);
        return;
    }
    if (execution_prefetch_control_) throw std::logic_error("Prefetch admission requires its executed stop and visibility boundaries");
    bind_prefetch_consumer_boundary(fact, scope, *op);
    op->visibility_dependency_required = false;

    if (op->hit_pages.size() < policy_.prefetch_threshold_pages()) {
        op->header.boundary_ts = fact.ts;
        op->header.boundary_epoch = op->header.consumer_epoch;
        op->policy_stop_ts = fact.ts;
        op->target_boundary_ts = fact.ts;
        op->timeout_deadline_ts = 0;
        op->policy_wait_duration_us = 0;
        op->service_consumer_dependency_required = false;
        cancel_prefetch_pending_release(fact, scope, *op, HiCachePrefetchState::Revoked);
        // Below-threshold work releases its reservation before extend allocation.
        drain_prefetch_pending_release(fact, scope, request_key);
        return;
    }

    const auto policy = policy_.prefetch_policy();
    auto progress = estimate_prefetch_progress(*op, fact);
    if (!progress.boundary_resolved) { return; }

    op->header.boundary_ts = progress.target_boundary_ts;
    op->header.boundary_epoch = op->header.consumer_epoch;
    op->completed_pages = std::move(progress.completed_pages);
    op->completed_byte_count = progress.completed_byte_count;
    op->policy_stop_ts = progress.policy_stop_ts;
    op->target_boundary_ts = progress.target_boundary_ts;
    op->timeout_deadline_ts = progress.timeout_deadline_ts;
    op->timed_out = progress.timed_out;
    op->policy_wait_duration_us = progress.policy_wait_duration_us;
    op->service_consumer_dependency_required = progress.service_consumer_dependency_required;
    op->visibility_dependency_required = progress.visibility_dependency_required;
    apply_prefetch_ready(fact, scope, *op);
}

/**
 * @brief Releases terminal-prefetch host reservation at a scheduler/extend boundary.
 */
void HiCacheState::drain_prefetch_pending_release(const HiCacheFact & fact, ScopedState & scope, const std::string & request_key) {
    if (execution_prefetch_control_) return; // Only a scheduler release event may free these buffers.
    const auto released_pages = scope.async_ops.release_prefetch_pending_host_pages_for_request(request_key);
    if (released_pages == 0) return;
    sync_capacity(scope, normalized_scope(fact), {});
}


} // namespace markov::trace_graph::modules::hicache::model
