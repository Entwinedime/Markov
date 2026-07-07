/**
 * @file
 * @brief HiCache target-derived storage prefetch lifecycle 建模。
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <set>
#include <utility>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

namespace {

constexpr uint64_t kStorageBatchPages = 128;
constexpr uint64_t kPrefetchStorageQueryBaseUs = 2000;
constexpr uint64_t kPrefetchStorageQueryPerPageUs = 1250;
constexpr uint64_t kPrefetchStorageLargeQueryPages = 16;
constexpr uint64_t kPrefetchStorageLargeQueryExtraUs = 2500;
constexpr uint64_t kPrefetchStorageVisibilityGuardUs = 2500;

uint64_t prefetch_storage_query_pages(uint64_t planned_pages, uint64_t hit_pages) {
    if (planned_pages == 0) return 0;
    if (hit_pages >= planned_pages) return planned_pages;
    const auto pages_until_first_miss = hit_pages + 1;
    const auto queried_batches = detail::ceil_div(pages_until_first_miss, kStorageBatchPages);
    return std::min(planned_pages, queried_batches * kStorageBatchPages);
}

uint64_t estimate_prefetch_storage_query_duration(uint64_t queried_pages) {
    if (queried_pages == 0) return 0;
    auto duration = kPrefetchStorageQueryBaseUs + queried_pages * kPrefetchStorageQueryPerPageUs;
    if (queried_pages > kPrefetchStorageLargeQueryPages) duration += kPrefetchStorageLargeQueryExtraUs;
    return duration;
}

} // namespace

/**
 * @brief 估计 storage prefetch 在当前 target boundary 已传输完成的 host prefix。
 *
 * 当前输入合同只告诉模型 prefetch candidate 和后续 cache extend 边界，不提供
 * 可校准的后台 I/O progress。因此 best-effort/timeout incomplete 的 completed prefix
 * 暂时为 0；wait-complete 或 terminal timeout 的“全量完成”由上层 policy 近似决定。
 */
HiCacheState::PrefetchIoProgressEstimate HiCacheState::estimate_prefetch_io_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact) const {
    (void)op;
    (void)fact;
    return PrefetchIoProgressEstimate{
        .completed_pages = {},
        .model_name = "zero_progress",
        .reason = "zero-progress IO model: state-model input has no calibrated storage transfer progress",
    };
}

/**
 * @brief 在一次 prefetch target boundary 上综合 stop policy、terminal 标记和 timeout。
 *
 * 返回值只回答“本 boundary 可见的 completed prefix 是多少”和“是否到达可 apply
 * 边界”；真正的 host radix materialize 由 apply_prefetch_ready 统一执行。
 */
HiCacheState::PrefetchProgressEstimate HiCacheState::estimate_prefetch_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact,
                                                                                bool terminal_boundary) const {
    auto estimate = PrefetchProgressEstimate{
        .storage_hit_pages = static_cast<uint64_t>(op.hit_pages.size()),
        .storage_hit_sufficient = op.hit_pages.size() >= policy_.prefetch_threshold_pages(),
        .terminal_boundary = terminal_boundary,
        .timeout_elapsed = policy_.prefetch_timeout_elapsed(op.header.enqueue_ts, fact.ts, static_cast<uint64_t>(op.planned_pages.size() * config_.page_size)),
    };
    if (!estimate.storage_hit_sufficient) {
        estimate.reason = "storage hit prefix is below target prefetch threshold";
        return estimate;
    }

    const auto policy = policy_.prefetch_policy();
    if (policy == "wait_complete") {
        estimate.completed_pages = op.hit_pages;
        estimate.fully_completed = true;
        estimate.reason = "wait_complete policy exposes all completed IO, prefetch is modeled as fully completed";
        return estimate;
    }
    if (policy == "best_effort") {
        const auto io_progress = estimate_prefetch_io_progress(op, fact);
        estimate.completed_pages = io_progress.completed_pages;
        estimate.reason = "best_effort can terminate immediately; " + io_progress.reason;
        return estimate;
    }
    if (policy == "timeout" && estimate.terminal_boundary && !estimate.timeout_elapsed) {
        estimate.completed_pages = op.hit_pages;
        estimate.fully_completed = true;
        estimate.reason = "terminal cache extend boundary is modeled as completed IO until calibrated progress is available";
        return estimate;
    }
    if (estimate.timeout_elapsed) {
        const auto io_progress = estimate_prefetch_io_progress(op, fact);
        estimate.completed_pages = io_progress.completed_pages;
        estimate.reason = "timeout stop boundary exposes only calibrated completed IO; " + io_progress.reason;
        return estimate;
    }
    estimate.reason = "prefetch has not reached a modeled completion or timeout boundary";
    return estimate;
}

/**
 * @brief 近似 storage-control revoke 的 release 相对 cache extend side effect 的可见性。
 *
 * C++ state model 不能消费 source observed/probe 结果。best-effort 的 below-threshold
 * revoke 来自 cache controller prefetch worker：只有当该 worker 已经完成本 op 的
 * storage hit query，最后一次 check_progress/cache_extend 边界才可能 drain 到对应的
 * release。这里不把 lookup 当成状态分界；唯一可改变状态的判定点是当前
 * cache_extend_input fact。
 */
bool HiCacheState::prefetch_release_visible_before_cache_extend(const HiCachePrefetchOperation & op, const HiCacheFact & boundary_fact) const {
    if (op.reserved_host_pages == 0) return false;
    if (op.hit_pages.size() >= policy_.prefetch_threshold_pages()) return false;
    if (policy_.prefetch_policy() != "best_effort") return true;
    if (op.storage_query_ready_ts == 0) return false;
    return op.storage_query_ready_ts + kPrefetchStorageVisibilityGuardUs <= boundary_fact.ts;
}

/**
 * @brief 根据 target policy 尝试 enqueue 一个 prefetch operation。
 *
 * 入口只消费 prefetch candidate path、当前 target memory/storage prefix、host capacity
 * 和 rate limit；source run 实际是否 prefetch 成功不能进入 target state。
 */
void HiCacheState::apply_prefetch_candidate_anchor(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions) {
    const auto resolution = token_directory_.resolve_prefetch_candidate_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "prefetch_lookup_touch");
    /**
     * @brief memory prefix 覆盖 device/host residency。
     *
     * storage-readable 不算 request 已可直接复用的 memory hit；storage hit 只用于后面的
     * prefetch I/O candidate。
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

    const auto allocation = request_host_allocation(fact, summary, transitions, scope, requested_pages, policy_.prefetch_threshold_pages(), true, "prefetch");
    const auto capacity = allocation.capacity_pages;
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
                                       .capacity_pages = capacity,
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
                                       .capacity_pages = capacity,
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
     * @brief hit_pages 是 L3 backend 中连续可读 prefix。
     *
     * 它决定 prefetch 是否值得发起，以及 terminate 后最多能 materialize 哪段 host-visible prefix。
     */
    const auto hit_pages = storage_hit_prefix(scope.storage, planned_projected_pages);
    const auto storage_query_pages = prefetch_storage_query_pages(effective_requested_pages, static_cast<uint64_t>(hit_pages.size()));
    const auto storage_query_start_ts = std::max(fact.ts, scope.prefetch_worker_available_ts);
    const auto storage_query_ready_ts = storage_query_start_ts + estimate_prefetch_storage_query_duration(storage_query_pages);
    scope.prefetch_worker_available_ts = storage_query_ready_ts;
    if (auto * prior = scope.async_ops.prefetch_for_request(request_key); prior != nullptr) {
        if (prefetch_active(*prior)) {
            if constexpr (debug_records_enabled())
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
                record_transition(fact, summary, transitions, "prefetch_suppressed", "prefetch", prior->planned_pages, before);
        }
    }

    HiCachePrefetchOperation op{
        .header = make_operation_header(HiCacheOperationKind::Prefetch,
                                        prefetch_id,
                                        normalized_scope(fact),
                                        request_key,
                                        owner,
                                        lookup.deepest_host_node,
                                        anchor_nodes,
                                        planned_pages,
                                        fact.ts,
                                        enqueue_epoch),
        .anchor_chain = anchor_nodes,
        .host_insert_pages = prefix_to(pages, memory_prefix.size() + hit_pages.size()),
        .host_visible_offset_pages = static_cast<uint64_t>(memory_prefix.size()),
        .planned_pages = planned_pages,
        .hit_pages = hit_pages,
        .requested_host_pages = effective_requested_pages,
        .reserved_host_pages = reservable,
        .storage_query_start_ts = storage_query_start_ts,
        .storage_query_ready_ts = storage_query_ready_ts,
        .priority = fact.priority,
        .prefetch_state = HiCachePrefetchState::Pending,
    };
    scope.async_ops.upsert_prefetch(std::move(op));
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
                                   .capacity_pages = capacity,
                                   .occupied_pages = allocation.occupied_pages,
                                   .reserved_pages = reservable,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .limit_pages = policy_.prefetch_capacity_limit_pages(),
                                   .pages = planned_pages,
                               });
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_reservation");
    if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "prefetch_planned", "prefetch", planned_pages, debug_state_digest());
}

/**
 * @brief 将已经完成的 prefetch prefix 统一 materialize 到 host radix。
 *
 * completed_pages 可能为空：这表示 target boundary 已经到达 stop 边界，但当前近似认为没有
 * 已完成 I/O。此时仍记录 lifecycle transition，并保留未完成 reservation 到后续 drain。
 */
void HiCacheState::apply_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                        HiCachePrefetchOperation & op) {
    const auto before_ready = debug_state_digest();
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             HiCachePrefetchState::Ready,
                                             HiCacheOperationState::Ready,
                                             "completed_prefetch_ready",
                                             fact.ts);
    if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "prefetch_ready", "prefetch", op.completed_pages, before_ready);
    if constexpr (debug_records_enabled()) {
        if (op.completed_pages.empty()) record_transition(fact, summary, transitions, "prefetch_terminated", "prefetch", op.planned_pages, before_ready);
    }

    const auto before_apply = debug_state_digest();
    const auto visible_pages = std::set<std::string>(op.completed_pages.begin(), op.completed_pages.end());
    (void)scope.tree.lookup(prefix_to(op.host_insert_pages, static_cast<size_t>(op.host_visible_offset_pages)));
    const auto host_insert_pages = prefix_to(op.host_insert_pages, static_cast<size_t>(op.host_visible_offset_pages + op.completed_pages.size()));
    auto insert = scope.tree.insert_host_path(host_insert_pages, visible_pages, true);
    sync_capacity_for_insert(scope, normalized_scope(fact), insert, "prefetch_insert_host");
    scope.storage.mark_readable_pages(normalized_scope(fact), op.completed_pages);
    for (const auto node_id : insert.touched_nodes) { scope.storage.mark_materialized_pages(scope.tree.node_pages(node_id), node_id); }
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             HiCachePrefetchState::Applied,
                                             HiCacheOperationState::Committed,
                                             "apply_host_visibility",
                                             fact.ts);
    /**
     * @brief completed prefix 立即转成 host residency。
     *
     * 未完成的 reservation 不在这里直接清零，而是作为 pending release 留给 request
     * cache extend / finalize 边界 drain。
     */
    op.reserved_host_pages = bounded_subtract(op.reserved_host_pages, static_cast<uint64_t>(op.completed_pages.size()));
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_apply_pending_host_release");
    if constexpr (debug_records_enabled())
        record_transition(fact,
                          summary,
                          transitions,
                          "apply_prefetch_host_visibility",
                          "L2",
                          flatten_node_pages(scope.tree, insert.new_host_nodes),
                          before_apply);
}

/**
 * @brief 取消 prefetch operation，但保留 pending host reservation 的 target-derived release。
 *
 * SGLang revoke/timeout incomplete 后 reservation 会经过后台队列和后续调度边界释放。
 * 模型用 reserved_host_pages 保留这段压力，避免在取消当场把 host capacity 过早释放。
 */
void HiCacheState::cancel_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                   ScopedState & scope, HiCachePrefetchOperation & op, const std::string & transition_kind,
                                                   HiCachePrefetchState prefetch_state) {
    const auto before = debug_state_digest();
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id, prefetch_state, HiCacheOperationState::Cancelled, transition_kind, fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_cancel_pending_host_release");
    if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, transition_kind, "prefetch", op.planned_pages, before);
}

/**
 * @brief 在 cache extend side effect 前结算同 request 的 active prefetch。
 *
 * 当前合同不再采集 source runtime checkpoint。模型把 request 进入
 * `cache_extend_input` 视为该 request prefetch 流程的 target-derived terminal
 * boundary，再由 target policy 决定 apply、revoke 或 timeout incomplete。
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
            record_policy_decision(fact,
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
        if constexpr (debug_records_enabled())
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
        op->completed_pages = std::move(progress.completed_pages);
        apply_prefetch_ready(fact, summary, transitions, scope, *op);
        return;
    }
    if (policy == "best_effort") {
        auto progress = estimate_prefetch_progress(*op, fact, terminal);
        if constexpr (debug_records_enabled())
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
        op->completed_pages = std::move(progress.completed_pages);
        apply_prefetch_ready(fact, summary, transitions, scope, *op);
        return;
    }
    if (policy == "timeout") {
        auto progress = estimate_prefetch_progress(*op, fact, terminal);
        if constexpr (debug_records_enabled())
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
        op->completed_pages = std::move(progress.completed_pages);
        apply_prefetch_ready(fact, summary, transitions, scope, *op);
        return;
    }
    op->completed_pages = op->hit_pages;
    if constexpr (debug_records_enabled())
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_cache_extend_boundary",
                                   .policy_name = policy,
                                   .decision = "apply_prefetch",
                                   .reason = "unknown prefetch policy falls back to immediate apply",
                                   .accepted = true,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .allocated_pages = static_cast<uint64_t>(op->completed_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
    apply_prefetch_ready(fact, summary, transitions, scope, *op);
}

/**
 * @brief 在 scheduler/extend 边界释放 terminal prefetch 的 pending host reservation。
 */
void HiCacheState::drain_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                  ScopedState & scope, const std::string & request_key, const std::string & capacity_reason,
                                                  const std::string & policy_reason) {
    uint64_t requested_host_pages = 0;
    uint64_t reserved_host_pages = 0;
    uint64_t released_operation_count = 0;
    std::string released_operation_id;
    std::vector<std::string> planned_pages;
    auto & prefetch_ops = scope.async_ops.prefetch_ops();
    for (const auto & operation_id : scope.async_ops.operations_for_request(request_key)) {
        auto prefetch = prefetch_ops.find(operation_id);
        if (prefetch == prefetch_ops.end() || prefetch_active(prefetch->second) || prefetch->second.reserved_host_pages == 0) continue;
        requested_host_pages += prefetch->second.requested_host_pages;
        reserved_host_pages += prefetch->second.reserved_host_pages;
        planned_pages.insert(planned_pages.end(), prefetch->second.planned_pages.begin(), prefetch->second.planned_pages.end());
        ++released_operation_count;
        released_operation_id = released_operation_count == 1 ? prefetch->second.header.operation_id : "multiple_prefetch_operations";
    }
    if (reserved_host_pages == 0) return;
    const auto released_pages = scope.async_ops.release_prefetch_pending_host_pages_for_request(request_key);
    if (released_pages == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, capacity_reason);
    if constexpr (debug_records_enabled())
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = released_operation_id,
                                   .policy_area = "host_allocation",
                                   .policy_name = "prefetch_host_release_queue",
                                   .decision = "drain_request_prefetch_pending_release_pages",
                                   .reason = policy_reason,
                                   .accepted = true,
                                   .requested_pages = requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(planned_pages.size()),
                                   .reserved_pages = reserved_host_pages,
                                   .allocator_released_pages = released_pages,
                                   .pages = planned_pages,
                               });
}


} // namespace markov::trace_graph::modules::hicache::model
