/**
 * @file
 * @brief HiCache canonical-radix state model 主链路。
 */
#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include "markov/trace_graph/core/dag_graph.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <ranges>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

/**
 * @brief 初始化 device allocator 的 count-level 投影。
 *
 * 该账本只模拟 SGLang allocator 的 free/release queue 可见性，不承担 page identity；
 * page identity 仍由 radix tree 和 capacity index 派生。
 */
void HiCacheState::DeviceAllocatorLedger::configure(uint64_t pages, bool sort_required) {
    if (initialized && capacity_pages == pages && need_sort == sort_required) return;
    initialized = true;
    need_sort = sort_required;
    capacity_pages = pages;
    free_pages = pages;
    release_pages = 0;
}

uint64_t HiCacheState::DeviceAllocatorLedger::available_pages() const { return free_pages + release_pages; }

/** @brief 判断本次 page 申请是否会触发 SGLang allocator eviction gate。 */
bool HiCacheState::DeviceAllocatorLedger::should_evict(uint64_t requested_pages) const {
    return initialized && capacity_pages > 0 && requested_pages > 0 && available_pages() < requested_pages;
}

/** @brief 把 pending release queue 合并回 allocator free pages。 */
void HiCacheState::DeviceAllocatorLedger::merge_release_pages() {
    free_pages += release_pages;
    release_pages = 0;
}

/**
 * @brief 模拟 SGLang 在 extend 前按 need_sort 条件同步 release queue。
 *
 * paged allocator 会把 batch overhead 纳入 pressure 判断；这里保留这个 count-level
 * 近似，避免 device cleanup 时机只由 final occupancy 决定。
 */
void HiCacheState::DeviceAllocatorLedger::merge_before_extend(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size) {
    if (!need_sort || page_size == 0) return;
    const auto needed_pages = page_size == 1 ? extend_tokens : extend_tokens / page_size + batch_size + 1;
    if (needed_pages > free_pages) merge_release_pages();
}

/** @brief 在真正分配 page 前，根据 requested_pages 判断 release queue 是否可见。 */
void HiCacheState::DeviceAllocatorLedger::merge_before_page_allocation(uint64_t requested_pages) {
    if (need_sort && requested_pages > free_pages) merge_release_pages();
}

bool HiCacheState::DeviceAllocatorLedger::can_allocate(uint64_t pages) const { return capacity_pages == 0 || pages <= free_pages; }

uint64_t HiCacheState::DeviceAllocatorLedger::allocate(uint64_t pages) {
    if (capacity_pages == 0) return pages;
    const auto consumed = std::min(pages, free_pages);
    free_pages -= consumed;
    return consumed;
}

/** @brief 释放 page 到 free pages 或 release queue，取决于 allocator need_sort。 */
uint64_t HiCacheState::DeviceAllocatorLedger::release(uint64_t pages) {
    if (pages == 0 || capacity_pages == 0) return 0;
    const auto room = capacity_pages > available_pages() ? capacity_pages - available_pages() : 0;
    const auto released = std::min(pages, room);
    if (need_sort) release_pages += released;
    else free_pages += released;
    return released;
}

HiCacheState::HiCacheState(HiCacheConfig config, std::unordered_set<size_t> terminal_prefetch_checkpoint_events)
    : config_(std::move(config)),
      pager_(config_),
      policy_(config_),
      terminal_prefetch_checkpoint_events_(std::move(terminal_prefetch_checkpoint_events)) {}

std::string HiCacheState::normalized_scope(const HiCacheFact & fact) const { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

std::string HiCacheState::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

HiCacheState::ScopedState & HiCacheState::scope_state(const HiCacheFact & fact) { return scopes_[normalized_scope(fact)]; }

void HiCacheState::ensure_device_allocator(ScopedState & scope) {
    scope.device_allocator.configure(policy_.l1_capacity_pages(), policy_.resolved().device_allocator_need_sort);
}

/**
 * @brief 判断新插入 device pages 是否会在 insert 边界后仍表现为 dirty。
 *
 * write-back 需要保留 dirty；write-through-selective 如果 threshold=1，会在同一 insert
 * 边界立即 backup，因此 final/transition 不应短暂暴露 dirty。
 */
bool HiCacheState::inserted_device_dirty_visible_at_insert_boundary() const {
    if (policy_.write_back_enabled()) return true;
    return policy_.write_count_enabled() && policy_.write_through_threshold() > 1;
}

/**
 * @brief 将 token path resolution 状态同步到 summary。
 *
 * 这里集中记录缺失/阶段错误/diagnostic counters，保证各 role handler 不各自解释
 * token directory 的错误语义。
 */
void HiCacheState::record_token_resolution(const HiCacheFact & fact, HiCacheSummary & summary, const HiCacheTokenResolution & resolution) const {
    const auto status = hicache_token_resolution_status_name(resolution.status);
    summary.token_resolution_by_status[status]++;
    summary.token_path_diagnostics[fact.role + "." + status]++;

    if (!resolution.ok()) {
        summary.missing_state_model_facts["token_resolution_" + status]++;
        if (fact.role == "request_lifecycle_anchor" && resolution.status == HiCacheTokenResolutionStatus::Missing)
            summary.token_path_diagnostics["lifecycle_anchor_missing_committed_path_count"]++;
        return;
    }

    if (fact.role == "prefetch_decision") summary.token_path_diagnostics["prefetch_path_not_committed_count"]++;
    if (fact.role == "request_lifecycle_anchor") {
        const auto * previous = token_directory_.previous_committed_snapshot(fact);
        if (previous != nullptr && resolution.page_aligned_token_count > previous->page_aligned_token_count)
            summary.token_path_diagnostics["lifecycle_path_growth_cross_page_boundary_count"]++;
    }
}

HiCachePagePath HiCacheState::page_path_from_resolution(const HiCacheFact & fact, const HiCacheTokenResolution & resolution) const {
    if (!resolution.ok() || resolution.tokens.empty()) return {};
    return pager_.project(fact, resolution.tokens);
}

std::string HiCacheState::digest() const { return derived_state().digest(); }

/** @brief 从每个 cache_scope 的 canonical runtime state 汇总最终可验证 state。 */
HiCacheDerivedStateSnapshot HiCacheState::derived_state(HiCacheDerivedStateMode mode) const {
    HiCacheDerivedStateView view(mode);
    for (const auto & scope : scopes_ | std::views::values) {
        view.include_tree(scope.tree);
        view.include_async(scope.async_ops);
        view.include_storage_directory(scope.storage);
    }
    return view.snapshot();
}

uint64_t HiCacheState::active_ref_owner_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.refs.active_owner_count(); }
    return count;
}

uint64_t HiCacheState::radix_split_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.tree.split_history().size(); }
    return count;
}

std::vector<HiCacheNodeSplitRecord> HiCacheState::radix_split_trace() const {
    std::vector<HiCacheNodeSplitRecord> splits;
    for (const auto & [scope_name, scope] : scopes_) {
        for (auto split : scope.tree.split_history()) {
            split.cache_scope = scope_name;
            splits.push_back(std::move(split));
        }
    }
    std::ranges::sort(splits, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.prefix_node != right.prefix_node) return left.prefix_node < right.prefix_node;
        return left.suffix_node < right.suffix_node;
    });
    return splits;
}

uint64_t HiCacheState::control_checkpoint_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.clock.checkpoint_count(); }
    return count;
}

std::vector<HiCacheControlCheckpoint> HiCacheState::control_checkpoint_trace() const {
    std::vector<HiCacheControlCheckpoint> checkpoints;
    for (const auto & scope : scopes_ | std::views::values) {
        const auto & scope_checkpoints = scope.clock.checkpoints();
        checkpoints.insert(checkpoints.end(), scope_checkpoints.begin(), scope_checkpoints.end());
    }
    std::ranges::sort(checkpoints, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.scheduler_epoch != right.scheduler_epoch) return left.scheduler_epoch < right.scheduler_epoch;
        return left.checkpoint_epoch < right.checkpoint_epoch;
    });
    return checkpoints;
}

uint64_t HiCacheState::async_lifecycle_transition_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.async_ops.lifecycle_transition_count(); }
    return count;
}

std::vector<HiCacheOperationLifecycleTransition> HiCacheState::async_lifecycle_trace() const {
    std::vector<HiCacheOperationLifecycleTransition> transitions;
    for (const auto & scope : scopes_ | std::views::values) {
        const auto & scope_transitions = scope.async_ops.lifecycle_transitions();
        transitions.insert(transitions.end(), scope_transitions.begin(), scope_transitions.end());
    }
    std::ranges::sort(transitions, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.transition_epoch != right.transition_epoch) return left.transition_epoch < right.transition_epoch;
        return left.operation_id < right.operation_id;
    });
    return transitions;
}

uint64_t HiCacheState::policy_decision_count() const { return static_cast<uint64_t>(policy_decisions_.size()); }

uint64_t HiCacheState::storage_known_page_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.storage.known_page_count(); }
    return count;
}

uint64_t HiCacheState::storage_readable_page_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.storage.readable_page_count(); }
    return count;
}

uint64_t HiCacheState::storage_backend_readable_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.storage.backend_readable_count(); }
    return count;
}

uint64_t HiCacheState::storage_materialized_page_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.storage.materialized_page_count(); }
    return count;
}

uint64_t HiCacheState::capacity_mutation_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.capacity.mutation_epoch(); }
    return count;
}

uint64_t HiCacheState::capacity_victim_choice_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.capacity.victim_choices().size(); }
    return count;
}

std::vector<HiCacheCapacityMutation> HiCacheState::capacity_mutation_trace() const {
    std::vector<HiCacheCapacityMutation> mutations;
    for (const auto & [scope_name, scope] : scopes_) {
        for (auto mutation : scope.capacity.mutation_trace()) {
            mutation.cache_scope = scope_name;
            mutations.push_back(std::move(mutation));
        }
    }
    std::ranges::sort(mutations, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.mutation_epoch != right.mutation_epoch) return left.mutation_epoch < right.mutation_epoch;
        return left.reason < right.reason;
    });
    return mutations;
}

std::vector<HiCacheCapacityVictimChoice> HiCacheState::capacity_victim_choices() const {
    std::vector<HiCacheCapacityVictimChoice> choices;
    for (const auto & [scope_name, scope] : scopes_) {
        for (auto choice : scope.capacity.victim_choices()) {
            choice.cache_scope = scope_name;
            choices.push_back(std::move(choice));
        }
    }
    std::ranges::sort(choices, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.selection_epoch != right.selection_epoch) return left.selection_epoch < right.selection_epoch;
        if (left.tier != right.tier) return left.tier < right.tier;
        return left.reason < right.reason;
    });
    return choices;
}

/** @brief 跨 scope 收集 capacity index 与 canonical tree 的一致性问题。 */
std::vector<HiCacheCapacityAuditIssue> HiCacheState::capacity_audit_issues() const {
    std::vector<HiCacheCapacityAuditIssue> issues;
    for (const auto & [scope_name, scope] : scopes_) {
        auto audit = scope.capacity.audit(scope.tree, scope.async_ops.reserved_pages(scope_name));
        for (auto issue : audit.issues) {
            issue.cache_scope = scope_name;
            issues.push_back(std::move(issue));
        }
    }
    std::ranges::sort(issues, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.node_id != right.node_id) return left.node_id < right.node_id;
        if (left.tier != right.tier) return left.tier < right.tier;
        return left.issue < right.issue;
    });
    return issues;
}

uint64_t HiCacheState::ref_mutation_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values) { count += scope.refs.mutation_trace().size(); }
    return count;
}

std::vector<HiCacheRefMutation> HiCacheState::ref_mutation_trace() const {
    std::vector<HiCacheRefMutation> mutations;
    for (const auto & [scope_name, scope] : scopes_) {
        for (auto mutation : scope.refs.mutation_trace()) {
            mutation.cache_scope = scope_name;
            mutations.push_back(std::move(mutation));
        }
    }
    std::ranges::sort(mutations, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.mutation_epoch != right.mutation_epoch) return left.mutation_epoch < right.mutation_epoch;
        if (left.owner_id != right.owner_id) return left.owner_id < right.owner_id;
        return left.action < right.action;
    });
    return mutations;
}

/** @brief 跨 scope 收集 ref ledger 与 canonical tree ref counter 的一致性问题。 */
std::vector<HiCacheRefAuditIssue> HiCacheState::ref_audit_issues() const {
    std::vector<HiCacheRefAuditIssue> issues;
    for (const auto & [scope_name, scope] : scopes_) {
        auto audit = scope.refs.audit(scope.tree);
        for (auto issue : audit.issues) {
            issue.cache_scope = scope_name;
            issues.push_back(std::move(issue));
        }
    }
    std::ranges::sort(issues, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.node_id != right.node_id) return left.node_id < right.node_id;
        if (left.ref_kind != right.ref_kind) return left.ref_kind < right.ref_kind;
        if (left.owner_id != right.owner_id) return left.owner_id < right.owner_id;
        return left.issue < right.issue;
    });
    return issues;
}

/**
 * @brief 将 canonical tree/ref/async reservation 变化同步到 capacity index。
 *
 * capacity index 是 eviction victim 和 summary 的派生索引；每次状态 mutation 后必须显式
 * 同步，避免 capacity handler 重新扫描整棵 tree 并引入隐藏状态源。
 */
void HiCacheState::sync_capacity(ScopedState & scope, const std::string & cache_scope, const std::vector<HiCacheNodeId> & node_ids,
                                 const std::string & reason) {
    const auto reserved = scope.async_ops.reserved_pages(cache_scope);
    if (node_ids.empty()) {
        (void)scope.capacity.sync_reservation(reserved, reason);
        return;
    }
    scope.refs.sync_tree_ref_copies(scope.tree, reason);
    (void)scope.capacity.sync_nodes(scope.tree, node_ids, reserved, reason);
}

/** @brief insert 可能影响 terminal、ancestor 和新建/恢复 node，需要合并成一次 capacity sync。 */
void HiCacheState::sync_capacity_for_insert(ScopedState & scope, const std::string & cache_scope, const HiCacheInsertResult & insert,
                                            const std::string & reason) {
    std::set<HiCacheNodeId> nodes;
    if (insert.terminal_node != 0) nodes.insert(insert.terminal_node);
    nodes.insert(insert.touched_nodes.begin(), insert.touched_nodes.end());
    nodes.insert(insert.new_device_nodes.begin(), insert.new_device_nodes.end());
    nodes.insert(insert.restored_device_nodes.begin(), insert.restored_device_nodes.end());
    nodes.insert(insert.dirtied_device_nodes.begin(), insert.dirtied_device_nodes.end());
    nodes.insert(insert.new_host_nodes.begin(), insert.new_host_nodes.end());
    sync_capacity(scope, cache_scope, { nodes.begin(), nodes.end() }, reason);
}

/** @brief ref mutation 影响 lock/host ref 计数，必须同步到 capacity victim eligibility。 */
void HiCacheState::sync_capacity_for_ref(ScopedState & scope, const std::string & cache_scope, const HiCacheRefMutation & mutation,
                                         const std::string & reason) {
    std::set<HiCacheNodeId> nodes;
    nodes.insert(mutation.lock_nodes.begin(), mutation.lock_nodes.end());
    nodes.insert(mutation.host_nodes.begin(), mutation.host_nodes.end());
    sync_capacity(scope, cache_scope, { nodes.begin(), nodes.end() }, reason);
}

/** @brief 给 policy decision 分配全局 epoch，并补齐 fact/source 归属字段。 */
void HiCacheState::record_policy_decision(const HiCacheFact & fact, HiCachePolicyDecisionRecord decision) {
    decision.decision_epoch = ++policy_decision_epoch_;
    decision.cache_scope = normalized_scope(fact);
    decision.request_key = scoped_request_key(fact);
    decision.role = fact.role;
    decision.event_name = fact.event_name;
    policy_decisions_.push_back(std::move(decision));
}

/**
 * @brief 写入一条 state transition，并可选记录 before/after digest。
 *
 * 空 page transition 默认不输出，除非它代表生命周期事件本身，例如 release_ref 或
 * prefetch suppressed/late。这样 transition trace 既能审计关键 lifecycle，又不会被无效
 * page set 刷屏。
 */
void HiCacheState::record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                     const std::string & kind, const std::string & tier, const std::vector<std::string> & pages,
                                     const std::string & before_digest) {
    if (pages.empty() && kind != "release_ref" && kind != "prefetch_suppressed" && kind != "prefetch_late") return;
    HiCacheStateTransition transition;
    transition.transition_id = std::to_string(summary.state_transition_count + transitions.size() + 1);
    transition.kind = kind;
    transition.role = fact.role;
    transition.request_id = fact.request_id;
    transition.operation_id = fact.operation_id;
    transition.event_name = fact.event_name;
    transition.cache_scope = normalized_scope(fact);
    transition.ts = fact.ts;
    transition.source_event_index = fact.source_event_index;
    transition.tier = tier;
    transition.pages = pages;
    if (config_.emit_state_digests) {
        transition.before_state_digest = before_digest;
        transition.after_state_digest = digest();
    }
    summary.transitions_by_kind[kind]++;
    transitions.push_back(std::move(transition));
}

/**
 * @brief 单个可消费 fact 的统一 dispatch 入口。
 *
 * 所有 fact 先进入 token directory，再在 target control 边界 drain write-through ACK；
 * 之后才按 role 推进具体状态机，保证多个 handler 看到一致的 token timeline 和 ref/capacity
 * 基线。
 */
std::vector<HiCacheStateTransition> HiCacheState::apply_fact(const HiCacheFact & fact, HiCacheFactRole role, HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    token_directory_.observe_fact_path(fact, pager_.page_size_for_fact(fact));
    if (role != HiCacheFactRole::Unknown) {
        auto & scope = scope_state(fact);
        drain_write_through_backup_refs(fact, summary, transitions, scope, "write_through_backup_ack_boundary");
    }

    switch (role) {
    case HiCacheFactRole::RequestBoundMatchAnchor:
        apply_request_bound_match_anchor(fact, summary, transitions);
        break;
    case HiCacheFactRole::RequestAdmission:
        apply_request_admission(fact, summary, transitions);
        break;
    case HiCacheFactRole::RequestLifecycleAnchor:
        apply_request_lifecycle_anchor(fact, summary, transitions);
        break;
    case HiCacheFactRole::PrefetchDecision:
        apply_prefetch_decision(fact, summary, transitions);
        break;
    case HiCacheFactRole::PrefetchCheckPoint:
        apply_prefetch_check_point(fact, summary, transitions);
        break;
    case HiCacheFactRole::Unknown:
        break;
    }
    return transitions;
}


} // namespace markov::trace_graph::modules::hicache::model
