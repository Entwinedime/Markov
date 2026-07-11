/**
 * @file
 * @brief Models the target-derived HiCache storage-prefetch lifecycle.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

namespace {

constexpr uint64_t kStorageBatchPages = 128;
constexpr uint64_t kPrefetchStorageQueryBaseUs = 2'000;
constexpr uint64_t kPrefetchStorageQueryPerPageUs = 1'250;
constexpr uint64_t kPrefetchStorageLargeQueryPages = 16;
constexpr uint64_t kPrefetchStorageLargeQueryExtraUs = 2'500;
constexpr uint64_t kPrefetchStorageVisibilityGuardUs = 2'500;

uint64_t prefetch_storage_query_pages(uint64_t planned_pages, uint64_t hit_pages) {
    if (planned_pages == 0) return 0;
    if (hit_pages >= planned_pages) return planned_pages;
    const auto pages_until_first_miss = hit_pages + 1;
    const auto queried_batches = detail::ceil_div(pages_until_first_miss, kStorageBatchPages);
    return std::min(planned_pages, core::checked_multiply_u64(queried_batches, kStorageBatchPages, "HiCache storage query page count exceeds uint64 range"));
}

uint64_t estimate_prefetch_storage_query_duration(uint64_t queried_pages) {
    if (queried_pages == 0) return 0;
    auto duration =
        core::checked_add_u64(kPrefetchStorageQueryBaseUs,
                              core::checked_multiply_u64(queried_pages, kPrefetchStorageQueryPerPageUs, "HiCache storage query duration exceeds uint64 range"),
                              "HiCache storage query duration exceeds uint64 range");
    if (queried_pages > kPrefetchStorageLargeQueryPages)
        duration = core::checked_add_u64(duration, kPrefetchStorageLargeQueryExtraUs, "HiCache storage query duration exceeds uint64 range");
    return duration;
}

} // namespace

/**
 * @brief Estimates host-prefix transfer completed by the current target boundary.
 *
 * The input contract exposes a prefetch candidate and later cache-extend boundary,
 * but no calibratable background-I/O progress. Incomplete best-effort and timeout
 * paths therefore report zero completed pages. Policy owns terminal full completion.
 */
HiCacheState::PrefetchIoProgressEstimate HiCacheState::estimate_prefetch_io_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact) const {
    (void)op;
    (void)fact;
    return PrefetchIoProgressEstimate{
        .completed_pages = {},
#ifdef DEBUG
        .reason = "zero-progress IO model: state-model input has no calibrated storage transfer progress",
#endif
    };
}

/**
 * @brief Combines stop policy, terminal state, and timeout at one target boundary.
 *
 * The result describes the completed prefix visible at this boundary and whether
 * application may occur. `apply_prefetch_ready` owns all host-radix materialization.
 */
HiCacheState::PrefetchProgressEstimate HiCacheState::estimate_prefetch_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact,
                                                                                bool terminal_boundary) const {
    auto estimate = PrefetchProgressEstimate{
        .completed_pages = {},
        .storage_hit_sufficient = op.hit_pages.size() >= policy_.prefetch_threshold_pages(),
        .terminal_boundary = terminal_boundary,
        .timeout_elapsed = policy_.prefetch_timeout_elapsed(HiCachePrefetchTimeoutObservation{
            .enqueue_ts = op.header.enqueue_ts,
            .boundary_ts = fact.ts,
            .token_count = core::checked_multiply_u64(static_cast<uint64_t>(op.planned_pages.size()),
                                                      config_.page_size,
                                                      "HiCache prefetch timeout token projection exceeds uint64 range"),
        }),
    };
    if (!estimate.storage_hit_sufficient) {
#ifdef DEBUG
        estimate.reason = "storage hit prefix is below target prefetch threshold";
#endif
        return estimate;
    }

    const auto policy = policy_.prefetch_policy();
    if (policy == "wait_complete") {
        estimate.completed_pages = op.hit_pages;
#ifdef DEBUG
        estimate.reason = "wait_complete policy exposes all completed IO, prefetch is modeled as fully completed";
#endif
        return estimate;
    }
    if (policy == "best_effort") {
        const auto io_progress = estimate_prefetch_io_progress(op, fact);
        estimate.completed_pages = io_progress.completed_pages;
#ifdef DEBUG
        estimate.reason = "best_effort can terminate immediately; " + io_progress.reason;
#endif
        return estimate;
    }
    if (policy == "timeout" && estimate.terminal_boundary && !estimate.timeout_elapsed) {
        estimate.completed_pages = op.hit_pages;
#ifdef DEBUG
        estimate.reason = "terminal cache extend boundary is modeled as completed IO until calibrated progress is available";
#endif
        return estimate;
    }
    if (estimate.timeout_elapsed) {
        const auto io_progress = estimate_prefetch_io_progress(op, fact);
        estimate.completed_pages = io_progress.completed_pages;
#ifdef DEBUG
        estimate.reason = "timeout stop boundary exposes only calibrated completed IO; " + io_progress.reason;
#endif
        return estimate;
    }
#ifdef DEBUG
    estimate.reason = "prefetch has not reached a modeled completion or timeout boundary";
#endif
    return estimate;
}

/**
 * @brief Approximates revoke-release visibility before cache-extend side effects.
 *
 * The C++ model cannot consume observed source/probe outcomes. A below-threshold
 * best-effort revoke originates from the controller prefetch worker, and can be
 * drained by the final progress/extend boundary only after that worker has completed
 * the operation's storage-hit query. Lookup is not a state boundary; the current
 * `cache_extend_input` fact remains the only state-changing decision point.
 */
bool HiCacheState::prefetch_release_visible_before_cache_extend(const HiCachePrefetchOperation & op, const HiCacheFact & boundary_fact) const {
    if (op.reserved_host_pages == 0) return false;
    if (op.hit_pages.size() >= policy_.prefetch_threshold_pages()) return false;
    if (policy_.prefetch_policy() != "best_effort") return true;
    if (op.storage_query_ready_ts == 0) return false;
    return boundary_fact.ts >= op.storage_query_ready_ts && boundary_fact.ts - op.storage_query_ready_ts >= kPrefetchStorageVisibilityGuardUs;
}

void HiCacheState::suppress_prior_prefetch(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                           const std::string & request_key) {
    auto * prior = scope.async_ops.prefetch_for_request(request_key);
    if (prior == nullptr || !prefetch_active(*prior)) return;
    if constexpr (debug_records_enabled()) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = prior->header.operation_id,
                                   .policy_area = "prefetch_enqueue",
                                   .policy_name = policy_.prefetch_policy(),
                                   .decision = "suppress_prior_prefetch",
                                   .reason = "new target prefetch supersedes existing active prefetch for the same request",
                                   .accepted = true,
                                   .requested_pages = prior->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(prior->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(prior->hit_pages.size()),
                                   .pages = prior->planned_pages,
                               });
    }
    const auto before = debug_state_digest();
    scope.async_ops.set_prefetch_state_by_id(prior->header.operation_id,
                                             HiCachePrefetchState::Suppressed,
                                             HiCacheOperationState::Cancelled,
                                             "superseded",
                                             fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, prior->header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_supersede_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_supersede_reservation");
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "prefetch_suppressed", .tier = "prefetch" }, prior->planned_pages, before);
}

/**
 * @brief Attempts to enqueue one prefetch under target policy.
 *
 * The decision consumes only the candidate path, target memory/storage prefixes,
 * host capacity, and target rate limit. Source-run success never enters target state.
 */
void HiCacheState::apply_prefetch_candidate_anchor(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions) {
    const auto resolution = token_directory_.resolve_prefetch_candidate_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    scope.storage.observe_path(page_path);
#ifdef DEBUG
    scope.tree.observe_page_path(page_path);
#endif
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
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "prefetch_enqueue",
                                       .policy_name = policy_.prefetch_policy(),
                                       .decision = "skip_prefetch",
                                       .reason = "target memory-visible prefix already covers request",
                                       .accepted = false,
                                       .candidate_pages = 0,
                                       .threshold_pages = policy_.prefetch_threshold_pages(),
                                       .pages = pages,
                                   });
        return;
    }

    const auto requested_pages = static_cast<uint64_t>(planned_pages.size());
    if (requested_pages < policy_.prefetch_threshold_pages()) {
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "prefetch_enqueue",
                                       .policy_name = policy_.prefetch_policy(),
                                       .decision = "skip_prefetch",
                                       .reason = "planned target pages are below prefetch threshold",
                                       .accepted = false,
                                       .requested_pages = requested_pages,
                                       .candidate_pages = requested_pages,
                                       .threshold_pages = policy_.prefetch_threshold_pages(),
                                       .pages = planned_pages,
                                   });
        return;
    }
    const auto active_requested_pages = scope.async_ops.active_requested_pages(normalized_scope(fact));
    if (policy_.prefetch_rate_limited(active_requested_pages)) {
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "prefetch_enqueue",
                                       .policy_name = policy_.prefetch_policy(),
                                       .decision = "skip_prefetch",
                                       .reason = "active requested prefetch pages reached target rate limit",
                                       .accepted = false,
                                       .requested_pages = requested_pages,
                                       .candidate_pages = requested_pages,
                                       .active_requested_pages = active_requested_pages,
                                       .threshold_pages = policy_.prefetch_threshold_pages(),
                                       .limit_pages = policy_.prefetch_capacity_limit_pages(),
                                       .pages = planned_pages,
                                   });
        return;
    }

    const auto request_key = scoped_request_key(fact);
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
                                                    summary,
                                                    transitions,
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
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "prefetch_enqueue",
                                       .policy_name = policy_.prefetch_policy(),
                                       .decision = "skip_prefetch",
                                       .reason = "target host capacity cannot fit a threshold-sized prefetch allocation after cleanup",
                                       .accepted = false,
                                       .requested_pages = requested_pages,
                                       .candidate_pages = requested_pages,
                                       .active_requested_pages = active_requested_pages,
                                       .capacity_pages = allocation.capacity_pages,
                                       .occupied_pages = allocation.occupied_pages,
                                       .reserved_pages = allocation.reserved_pages,
                                       .threshold_pages = policy_.prefetch_threshold_pages(),
                                       .limit_pages = policy_.prefetch_capacity_limit_pages(),
                                       .pages = planned_pages,
                                   });
        return;
    }
    auto reservable = allocation.accepted_pages;
    if (allocation.truncated) {
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "prefetch_enqueue",
                                       .policy_name = policy_.prefetch_policy(),
                                       .decision = "truncate_prefetch_reservation",
                                       .reason = "host allocator retry allows only a threshold-sized prefix of planned prefetch pages",
                                       .accepted = true,
                                       .requested_pages = requested_pages,
                                       .candidate_pages = reservable,
                                       .active_requested_pages = active_requested_pages,
                                       .capacity_pages = allocation.capacity_pages,
                                       .occupied_pages = allocation.occupied_pages,
                                       .reserved_pages = allocation.reserved_pages,
                                       .threshold_pages = policy_.prefetch_threshold_pages(),
                                       .limit_pages = policy_.prefetch_capacity_limit_pages(),
                                       .pages = planned_pages,
                                   });
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
    const auto storage_query_pages = prefetch_storage_query_pages(effective_requested_pages, static_cast<uint64_t>(hit_pages.size()));
    const auto storage_query_start_ts = std::max(fact.ts, scope.prefetch_worker_available_ts);
    const auto storage_query_ready_ts = core::checked_add_u64(storage_query_start_ts,
                                                              estimate_prefetch_storage_query_duration(storage_query_pages),
                                                              "HiCache storage query ready timestamp exceeds uint64 range");
    scope.prefetch_worker_available_ts = storage_query_ready_ts;
    suppress_prior_prefetch(fact, summary, transitions, scope, request_key);

    HiCachePrefetchOperation op{
        .header = make_operation_header(HiCacheOperationKind::Prefetch,
                                        prefetch_id,
                                        normalized_scope(fact),
                                        request_key,
                                        owner,
                                        planned_pages,
                                        fact.ts,
                                        enqueue_epoch),
        .host_insert_pages = prefix_to(pages, memory_prefix.size() + hit_pages.size()),
        .host_visible_offset_pages = static_cast<uint64_t>(memory_prefix.size()),
        .planned_pages = planned_pages,
        .hit_pages = hit_pages,
        .completed_pages = {},
        .requested_host_pages = effective_requested_pages,
        .reserved_host_pages = reservable,
        .storage_query_ready_ts = storage_query_ready_ts,
        .prefetch_state = HiCachePrefetchState::Pending,
    };
    scope.async_ops.insert_prefetch(std::move(op));
    if constexpr (debug_records_enabled())
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = prefetch_id,
                                   .policy_area = "prefetch_enqueue",
                                   .policy_name = policy_.prefetch_policy(),
                                   .decision = "enqueue_prefetch",
                                   .reason = "planned target pages pass threshold, rate limit, and host reservation checks",
                                   .accepted = true,
                                   .requested_pages = effective_requested_pages,
                                   .candidate_pages = effective_requested_pages,
                                   .hit_pages = static_cast<uint64_t>(hit_pages.size()),
                                   .active_requested_pages = active_requested_pages,
                                   .capacity_pages = allocation.capacity_pages,
                                   .occupied_pages = allocation.occupied_pages,
                                   .reserved_pages = reservable,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .limit_pages = policy_.prefetch_capacity_limit_pages(),
                                   .pages = planned_pages,
                               });
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_reservation");
    if constexpr (debug_records_enabled())
        record_transition(fact,
                          summary,
                          transitions,
                          TransitionDescriptor{ .kind = "prefetch_planned", .tier = "prefetch" },
                          planned_pages,
                          debug_state_digest());
}

/**
 * @brief Materializes a completed prefetch prefix into the host radix.
 *
 * `completed_pages` may be empty when a stop boundary is reached before the current
 * approximation observes completed I/O. Lifecycle still advances, while outstanding
 * reservation remains pending for a later drain.
 */
void HiCacheState::apply_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                        HiCachePrefetchOperation & op) {
    const auto before_ready = debug_state_digest();
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             HiCachePrefetchState::Ready,
                                             HiCacheOperationState::Ready,
                                             "completed_prefetch_ready",
                                             fact.ts);
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "prefetch_ready", .tier = "prefetch" }, op.completed_pages, before_ready);
    if constexpr (debug_records_enabled()) {
        if (op.completed_pages.empty())
            record_transition(fact,
                              summary,
                              transitions,
                              TransitionDescriptor{ .kind = "prefetch_terminated", .tier = "prefetch" },
                              op.planned_pages,
                              before_ready);
    }

    const auto before_apply = debug_state_digest();
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
#ifdef DEBUG
    for (const auto node_id : insert.touched_nodes) { scope.storage.mark_materialized_pages(scope.tree.node_pages(node_id), node_id); }
#endif
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             HiCachePrefetchState::Applied,
                                             HiCacheOperationState::Committed,
                                             "apply_host_visibility",
                                             fact.ts);
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
    if constexpr (debug_records_enabled())
        record_transition(fact,
                          summary,
                          transitions,
                          TransitionDescriptor{ .kind = "apply_prefetch_host_visibility", .tier = "L2" },
                          flatten_node_pages(scope.tree, insert.new_host_nodes),
                          before_apply);
}

/**
 * @brief Cancels a prefetch while retaining target-derived pending host reservation.
 *
 * After revoke or incomplete timeout, SGLang releases reservation through background
 * queues and a later scheduling boundary. `reserved_host_pages` retains that pressure
 * so cancellation does not free host capacity prematurely.
 */
void HiCacheState::cancel_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                   ScopedState & scope, HiCachePrefetchOperation & op, const std::string & transition_kind,
                                                   HiCachePrefetchState prefetch_state) {
    const auto before = debug_state_digest();
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id, prefetch_state, HiCacheOperationState::Cancelled, transition_kind, fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_cancel_pending_host_release");
    if constexpr (debug_records_enabled())
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = transition_kind, .tier = "prefetch" }, op.planned_pages, before);
}

/**
 * @brief Settles the request's active prefetch before cache-extend side effects.
 *
 * The current contract does not capture source runtime checkpoints. A request's
 * `cache_extend_input` is therefore its target-derived terminal prefetch boundary,
 * where target policy selects apply, revoke, or incomplete timeout.
 */
void HiCacheState::settle_prefetch_before_cache_extend(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                       ScopedState & scope, const std::string & request_key) {
    auto * op = scope.async_ops.prefetch_for_request(request_key);
    if (op == nullptr || !prefetch_active(*op)) {
        if constexpr (debug_records_enabled())
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .policy_area = "prefetch_cache_extend_boundary",
                                       .policy_name = policy_.prefetch_policy(),
                                       .decision = "skip_prefetch_settle",
                                       .reason = "no active target prefetch operation for request",
                                       .accepted = false,
                                   });
        return;
    }
    op->header.boundary_ts = fact.ts;
    constexpr bool terminal = true;

    auto suppress = [&](const std::string & kind, HiCachePrefetchState state) {
        cancel_prefetch_pending_release(fact, summary, transitions, scope, *op, kind, state);
    };

    if (op->hit_pages.size() < policy_.prefetch_threshold_pages()) {
        op->release_before_cache_extend = prefetch_release_visible_before_cache_extend(*op, fact);
        if constexpr (debug_records_enabled())
            record_policy_decision(
                fact,
                HiCachePolicyDecisionRecord{
                    .operation_id = op->header.operation_id,
                    .policy_area = "prefetch_cache_extend_boundary",
                    .policy_name = policy_.prefetch_policy(),
                    .decision = "revoke_prefetch",
                    .reason = op->release_before_cache_extend
                                  ? "storage readable prefix is below prefetch threshold; prefetch worker query is ready before cache extend"
                                  : "storage readable prefix is below prefetch threshold; prefetch worker query is not ready before cache extend",
                    .accepted = false,
                    .requested_pages = op->requested_host_pages,
                    .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                    .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                    .reserved_pages = op->reserved_host_pages,
                    .threshold_pages = policy_.prefetch_threshold_pages(),
                    .pages = op->planned_pages,
                });
        suppress("prefetch_revoked", HiCachePrefetchState::Revoked);
        return;
    }

    const auto policy = policy_.prefetch_policy();
    if (policy == "wait_complete") {
        auto progress = estimate_prefetch_progress(*op, fact, terminal);
#ifdef DEBUG
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_cache_extend_boundary",
                                   .policy_name = policy,
                                   .decision = "apply_prefetch",
                                   .reason = progress.reason,
                                   .accepted = true,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .allocated_pages = static_cast<uint64_t>(progress.completed_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
#endif
        op->completed_pages = std::move(progress.completed_pages);
        apply_prefetch_ready(fact, summary, transitions, scope, *op);
        return;
    }
    if (policy == "best_effort") {
        auto progress = estimate_prefetch_progress(*op, fact, terminal);
#ifdef DEBUG
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_cache_extend_boundary",
                                   .policy_name = policy,
                                   .decision = "terminate_prefetch",
                                   .reason = progress.reason,
                                   .accepted = true,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .allocated_pages = static_cast<uint64_t>(progress.completed_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
#endif
        op->completed_pages = std::move(progress.completed_pages);
        apply_prefetch_ready(fact, summary, transitions, scope, *op);
        return;
    }
    if (policy == "timeout") {
        auto progress = estimate_prefetch_progress(*op, fact, terminal);
#ifdef DEBUG
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_cache_extend_boundary",
                                   .policy_name = policy,
                                   .decision = "apply_prefetch",
                                   .reason = progress.reason,
                                   .accepted = true,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .allocated_pages = static_cast<uint64_t>(progress.completed_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
#endif
        op->completed_pages = std::move(progress.completed_pages);
        apply_prefetch_ready(fact, summary, transitions, scope, *op);
        return;
    }
    throw std::logic_error("Resolved HiCache prefetch policy is not executable: " + policy);
}

/**
 * @brief Releases terminal-prefetch host reservation at a scheduler/extend boundary.
 */
void HiCacheState::drain_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                  ScopedState & scope, const std::string & request_key, const PrefetchReleaseReasons & reasons) {
#ifdef DEBUG
    uint64_t requested_host_pages = 0;
    uint64_t reserved_host_pages = 0;
    uint64_t released_operation_count = 0;
    std::string released_operation_id;
    std::vector<std::string> planned_pages;
    auto & prefetch_ops = scope.async_ops.prefetch_ops();
    for (const auto & operation_id : scope.async_ops.operations_for_request(request_key)) {
        auto prefetch = prefetch_ops.find(operation_id);
        if (prefetch == prefetch_ops.end() || prefetch_active(prefetch->second) || prefetch->second.reserved_host_pages == 0) continue;
        requested_host_pages =
            core::checked_add_u64(requested_host_pages, prefetch->second.requested_host_pages, "HiCache drained prefetch request pages exceed uint64 range");
        reserved_host_pages =
            core::checked_add_u64(reserved_host_pages, prefetch->second.reserved_host_pages, "HiCache drained prefetch reservation pages exceed uint64 range");
        planned_pages.insert(planned_pages.end(), prefetch->second.planned_pages.begin(), prefetch->second.planned_pages.end());
        (void)core::checked_increment_u64(released_operation_count, "HiCache drained prefetch operation count exceeds uint64 range");
        released_operation_id = released_operation_count == 1 ? prefetch->second.header.operation_id : "multiple_prefetch_operations";
    }
    if (reserved_host_pages == 0) return;
#endif
    const auto released_pages = scope.async_ops.release_prefetch_pending_host_pages_for_request(request_key);
    if (released_pages == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, std::string(reasons.capacity));
#ifdef DEBUG
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .operation_id = released_operation_id,
                               .policy_area = "host_allocation",
                               .policy_name = "prefetch_host_release_queue",
                               .decision = "drain_request_prefetch_pending_release_pages",
                               .reason = std::string(reasons.policy),
                               .accepted = true,
                               .requested_pages = requested_host_pages,
                               .candidate_pages = static_cast<uint64_t>(planned_pages.size()),
                               .reserved_pages = reserved_host_pages,
                               .allocator_released_pages = released_pages,
                               .pages = planned_pages,
                           });
#else
    (void)summary;
    (void)transitions;
    (void)reasons;
#endif
}


} // namespace markov::trace_graph::modules::hicache::model
