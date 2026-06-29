/**
 * @file
 * @brief HiCache target-derived storage prefetch lifecycle 建模。
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
 * @brief 估计 storage prefetch 在当前 checkpoint 已传输完成的 host prefix。
 *
 * 当前输入合同只告诉模型 check_prefetch_progress 的位置和 storage hit prefix，不提供
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
 * @brief 在一次 prefetch checkpoint 上综合 stop policy、terminal 标记和 timeout。
 *
 * 返回值只回答“本 checkpoint 可见的 completed prefix 是多少”和“是否到达可 apply
 * 边界”；真正的 host radix materialize 由 apply_prefetch_ready 统一执行。
 */
HiCacheState::PrefetchProgressEstimate HiCacheState::estimate_prefetch_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact,
                                                                                bool terminal_checkpoint) const {
    auto estimate = PrefetchProgressEstimate{
        .storage_hit_pages = static_cast<uint64_t>(op.hit_pages.size()),
        .storage_hit_sufficient = op.hit_pages.size() >= policy_.prefetch_threshold_pages(),
        .terminal_checkpoint = terminal_checkpoint,
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
    if (policy == "timeout" && estimate.terminal_checkpoint && !estimate.timeout_elapsed) {
        estimate.completed_pages = op.hit_pages;
        estimate.fully_completed = true;
        estimate.reason = "terminal timeout checkpoint is modeled as completed IO until calibrated progress is available";
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
 * @brief 判断当前 checkpoint 是否是同 request prefetch 流程的最后一个 checkpoint。
 *
 * 该集合来自建模前对 input timeline 的预扫描；它不读取 source actual progress，只利用
 * target-model 可消费 checkpoint 的相对顺序，避免 timeout policy 在普通中间 checkpoint
 * 上过早 apply。
 */
bool HiCacheState::terminal_prefetch_checkpoint(const HiCacheFact & fact) const {
    return terminal_prefetch_checkpoint_events_.contains(fact.source_event_index);
}

/**
 * @brief 根据 target policy 尝试 enqueue 一个 prefetch operation。
 *
 * 入口只消费 prefetch candidate path、当前 target memory/storage prefix、host capacity
 * 和 rate limit；source run 实际是否 prefetch 成功不能进入 target state。
 */
void HiCacheState::apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto resolution = token_directory_.resolve_prefetch_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "prefetch_lookup_touch");
    /*
     * memory prefix 覆盖 device/host residency，不把 storage-readable 算成 request
     * 已可直接复用的 memory hit；storage hit 只用于后面的 prefetch I/O candidate。
     */
    const auto memory_prefix = scope.tree.contiguous_prefix(pages, true, true, false);
    auto planned_pages = suffix_from(pages, memory_prefix.size());
    auto planned_projected_pages = suffix_from(page_path.pages, memory_prefix.size());
    if (planned_pages.empty()) {
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

    /*
     * hit_pages 是 L3 backend 中连续可读 prefix。它决定 prefetch 是否值得发起，以及
     * terminate 后最多能 materialize 哪段 host-visible prefix。
     */
    const auto hit_pages = storage_hit_prefix(scope.storage, planned_projected_pages);
    if (auto * prior = scope.async_ops.prefetch_for_request(request_key); prior != nullptr) {
        if (prefetch_active(*prior)) {
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
            const auto before = digest();
            scope.async_ops.set_prefetch_state_by_id(prior->header.operation_id,
                                                     HiCachePrefetchState::Suppressed,
                                                     HiCacheOperationState::Cancelled,
                                                     "superseded",
                                                     fact.ts);
            const auto ref = scope.refs.release_owner(scope.tree, prior->header.owner);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_supersede_ref_release");
            sync_capacity(scope, normalized_scope(fact), {}, "prefetch_supersede_reservation");
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
        .priority = fact.priority,
        .prefetch_state = HiCachePrefetchState::Pending,
    };
    scope.async_ops.upsert_prefetch(std::move(op));
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
    record_transition(fact, summary, transitions, "prefetch_planned", "prefetch", planned_pages, digest());
}

/**
 * @brief 将已经完成的 prefetch prefix 统一 materialize 到 host radix。
 *
 * completed_pages 可能为空：这表示 checkpoint 已经到达 stop 边界，但当前近似认为没有
 * 已完成 I/O。此时仍记录 lifecycle transition，并保留未完成 reservation 到后续 drain。
 */
void HiCacheState::apply_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                        ScopedState & scope, HiCachePrefetchOperation & op) {
    const auto before_ready = digest();
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id,
                                             HiCachePrefetchState::Ready,
                                             HiCacheOperationState::Ready,
                                             "completed_prefetch_ready",
                                             fact.ts);
    record_transition(fact, summary, transitions, "prefetch_ready", "prefetch", op.completed_pages, before_ready);
    if (op.completed_pages.empty()) record_transition(fact, summary, transitions, "prefetch_terminated", "prefetch", op.planned_pages, before_ready);

    const auto before_apply = digest();
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
    /*
     * completed prefix 立即转成 host residency；未完成的 reservation 不在这里直接清零，
     * 而是作为 pending release 留给 request admission / finalize 边界 drain。
     */
    op.reserved_host_pages = bounded_subtract(op.reserved_host_pages, static_cast<uint64_t>(op.completed_pages.size()));
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_apply_pending_host_release");
    record_transition(fact, summary, transitions, "apply_prefetch_host_visibility", "L2", flatten_node_pages(scope.tree, insert.new_host_nodes), before_apply);
}

/**
 * @brief 取消 prefetch operation，但保留 pending host reservation 的 target-derived release。
 *
 * SGLang revoke/timeout incomplete 后 reservation 会经过后台队列和后续调度边界释放。
 * 模型用 reserved_host_pages 保留这段压力，避免在取消当场把 host capacity 过早释放。
 */
void HiCacheState::cancel_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                   ScopedState & scope, HiCachePrefetchOperation & op, const std::string & transition_kind,
                                                   HiCachePrefetchState prefetch_state) {
    const auto before = digest();
    scope.async_ops.set_prefetch_state_by_id(op.header.operation_id, prefetch_state, HiCacheOperationState::Cancelled, transition_kind, fact.ts);
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_cancel_pending_host_release");
    record_transition(fact, summary, transitions, transition_kind, "prefetch", op.planned_pages, before);
}

/**
 * @brief 在 check_prefetch_progress checkpoint 上推进 active prefetch。
 *
 * checkpoint 本身只提供 target scheduler 锚点；是否 apply/revoke/timeout 由 target policy、
 * terminal checkpoint 预扫描和当前 storage hit/progress 近似共同决定。
 */
void HiCacheState::apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto & scope = scope_state(fact);
    const auto request_key = scoped_request_key(fact);
    auto * op = scope.async_ops.prefetch_for_request(request_key);
    if (op == nullptr || !prefetch_active(*op)) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "prefetch_checkpoint",
                                   .policy_name = policy_.prefetch_policy(),
                                   .decision = "skip_checkpoint",
                                   .reason = "no active target prefetch operation for request",
                                   .accepted = false,
                               });
        return;
    }
    const auto terminal = terminal_prefetch_checkpoint(fact);
    const auto checkpoint = scope.clock.record_target_checkpoint(normalized_scope(fact), request_key, fact.ts, terminal, fact.source_event_index);
    op->header.checkpoint_epoch = checkpoint.checkpoint_epoch;
    op->header.checkpoint_ts = fact.ts;

    auto suppress = [&](const std::string & kind, HiCachePrefetchState state) {
        cancel_prefetch_pending_release(fact, summary, transitions, scope, *op, kind, state);
    };

    if (op->hit_pages.size() < policy_.prefetch_threshold_pages()) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_checkpoint",
                                   .policy_name = policy_.prefetch_policy(),
                                   .decision = "revoke_prefetch",
                                   .reason = "storage readable prefix is below prefetch threshold",
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
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_checkpoint",
                                   .policy_name = policy,
                                   .decision = terminal ? "apply_prefetch" : "wait_for_completion",
                                   .reason = terminal ? progress.reason : "wait_complete requires a checkpoint where completed IO is known",
                                   .accepted = terminal,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .allocated_pages = static_cast<uint64_t>(progress.completed_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
        if (terminal) {
            op->completed_pages = std::move(progress.completed_pages);
            apply_prefetch_ready(fact, summary, transitions, scope, *op);
        }
        return;
    }
    if (policy == "best_effort") {
        auto progress = estimate_prefetch_progress(*op, fact, terminal);
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_checkpoint",
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
        const auto timeout_elapsed =
            policy_.prefetch_timeout_elapsed(op->header.enqueue_ts, fact.ts, static_cast<uint64_t>(op->planned_pages.size() * config_.page_size));
        auto progress = estimate_prefetch_progress(*op, fact, terminal);
        if (timeout_elapsed && !terminal) {
            record_policy_decision(fact,
                                   HiCachePolicyDecisionRecord{
                                       .operation_id = op->header.operation_id,
                                       .policy_area = "prefetch_checkpoint",
                                       .policy_name = policy,
                                       .decision = "cancel_prefetch_without_completed_io",
                                       .reason = progress.reason,
                                       .accepted = false,
                                       .requested_pages = op->requested_host_pages,
                                       .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                       .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                       .allocated_pages = static_cast<uint64_t>(progress.completed_pages.size()),
                                       .reserved_pages = op->reserved_host_pages,
                                       .threshold_pages = policy_.prefetch_threshold_pages(),
                                       .pages = op->planned_pages,
                                   });
            suppress("prefetch_timeout_incomplete", HiCachePrefetchState::Late);
            return;
        }
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_checkpoint",
                                   .policy_name = policy,
                                   .decision = terminal || timeout_elapsed ? "apply_prefetch" : "wait_for_timeout_or_completion",
                                   .reason = terminal || timeout_elapsed ? progress.reason : "timeout policy waits for terminal checkpoint or elapsed timeout",
                                   .accepted = terminal || timeout_elapsed,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .allocated_pages = static_cast<uint64_t>(progress.completed_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
        if (terminal || timeout_elapsed) {
            op->completed_pages = std::move(progress.completed_pages);
            apply_prefetch_ready(fact, summary, transitions, scope, *op);
        }
        return;
    }
    op->completed_pages = op->hit_pages;
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .operation_id = op->header.operation_id,
                               .policy_area = "prefetch_checkpoint",
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


} // namespace markov::trace_graph::modules::hicache::model
