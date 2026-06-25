/**
 * @file
 * @brief HiCache canonical-radix state model 主链路。
 */
#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <ranges>
#include <set>
#include <sstream>
#include <utility>

namespace TraceGraph {

namespace {

std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::vector<std::string> flatten_node_pages(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & nodes) {
    std::vector<std::string> pages;
    std::ranges::for_each(nodes, [&](auto node_id) {
        auto node_pages = tree.node_pages(node_id);
        pages.insert(pages.end(), node_pages.begin(), node_pages.end());
    });
    return pages;
}

bool has_host_backup(const HiCacheCacheNode & node) { return node.residency.host_present; }

template <typename T> std::vector<T> suffix_from(const std::vector<T> & values, size_t begin) {
    if (begin >= values.size()) return {};
    std::vector<T> result;
    result.reserve(values.size() - begin);
    auto view = values | std::views::drop(static_cast<std::ranges::range_difference_t<std::vector<T>>>(begin));
    std::ranges::copy(view, std::back_inserter(result));
    return result;
}

std::vector<std::string> prefix_to(const std::vector<std::string> & pages, size_t end) {
    end = std::min(end, pages.size());
    std::vector<std::string> result;
    result.reserve(end);
    auto view = pages | std::views::take(static_cast<std::ranges::range_difference_t<std::vector<std::string>>>(end));
    std::ranges::copy(view, std::back_inserter(result));
    return result;
}

std::vector<std::string> storage_hit_prefix(const HiCacheStorageDirectory & storage, const std::vector<HiCacheProjectedPage> & planned_pages) {
    return storage.contiguous_readable_prefix(planned_pages);
}

bool prefetch_active(const HiCachePrefetchOperation & op) {
    return op.prefetch_state == HiCachePrefetchState::Pending || op.prefetch_state == HiCachePrefetchState::Ready;
}

uint64_t ceil_div(uint64_t value, uint64_t divisor) {
    if (divisor == 0) return 0;
    return value / divisor + (value % divisor == 0 ? 0 : 1);
}

/**
 * @brief 一次 SGLang extend allocator batch 的语义化输入。
 *
 * 当前 trace 还没有 `ScheduleBatch` 粒度 invariant，因此调用方显式传入
 * resolved policy 中的 `batch_size=1`。结构体保留 batch 语义字段，后续接入
 * `extend_allocation_intent` 后只需要替换构造来源，不需要重写 capacity 链路。
 */
struct ExtendAllocationIntent {
    uint64_t batch_size = 1;
    uint64_t page_size = 0;
    uint64_t seq_tokens = 0;
    uint64_t prefix_tokens = 0;
    uint64_t extend_tokens = 0;
    uint64_t requested_tokens = 0;
    uint64_t requested_pages = 0;
    uint64_t allocated_pages = 0;

    /** @brief 本轮是否会形成 allocator pressure。 */
    [[nodiscard]] bool needs_pressure() const { return requested_pages > 0; }
};

/**
 * @brief 计算 SGLang paged extend 的 eviction pressure token 数。
 *
 * `alloc_paged_token_slots_extend()` 对 paged allocator 使用
 * `extend_num_tokens + len(seq_lens_cpu) * page_size`。当 `page_size == 1` 时，
 * SGLang 走 non-paged `alloc_token_slots()`，没有每 request 一页的保守预算。
 */
uint64_t extend_requested_tokens(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size) {
    if (extend_tokens == 0 || page_size == 0) return 0;
    if (page_size == 1) return extend_tokens;
    return extend_tokens + batch_size * page_size;
}

/**
 * @brief 计算本次 extend 真正占用的新 page 数。
 *
 * 该值对应 SGLang `get_num_new_pages(seq_lens, prefix_lens)`，不包含 allocator
 * 为 eviction gate 额外加入的 conservative batch overhead。
 */
uint64_t extend_allocated_pages(uint64_t seq_tokens, uint64_t prefix_tokens, uint64_t page_size) {
    if (seq_tokens == 0 || page_size == 0) return 0;
    const auto bounded_prefix_tokens = std::min(prefix_tokens, seq_tokens);
    const auto pages_after = ceil_div(seq_tokens, page_size);
    const auto pages_before = ceil_div(bounded_prefix_tokens, page_size);
    return pages_after > pages_before ? pages_after - pages_before : 0;
}

/**
 * @brief 用当前显式 single-request batch 合同构造 extend allocation intent。
 */
ExtendAllocationIntent make_extend_allocation_intent(uint64_t token_count, uint64_t prefix_tokens, uint64_t page_size, uint64_t batch_size) {
    auto intent = ExtendAllocationIntent{
        .batch_size = batch_size == 0 ? uint64_t{ 1 } : batch_size,
        .page_size = page_size,
        .seq_tokens = token_count,
    };
    if (token_count == 0 || page_size == 0) return intent;

    intent.prefix_tokens = std::min(token_count, prefix_tokens);
    intent.extend_tokens = token_count - intent.prefix_tokens;
    intent.requested_tokens = extend_requested_tokens(intent.extend_tokens, intent.batch_size, page_size);
    intent.requested_pages = ceil_div(intent.requested_tokens, page_size);
    intent.allocated_pages = extend_allocated_pages(intent.seq_tokens, intent.prefix_tokens, page_size);
    return intent;
}

/**
 * @brief 计算 SGLang allocator 语义下本轮需要真实清理的 page 数。
 *
 * SGLang 在 device/host allocator 当前可用空间不足时才调用 radix eviction；
 * 一旦触发，预算使用本次 allocation request 大小，而不是仅清理 free-space deficit。
 * 如果没有 allocation request，则只清理当前已经超过 target capacity 的部分。
 */
uint64_t allocation_cleanup_target(uint64_t occupied_pages, uint64_t reserved_pages, uint64_t capacity_pages, uint64_t requested_pages) {
    if (capacity_pages == 0) return 0;

    const auto committed_pages = occupied_pages + reserved_pages;
    const auto excess_pages = committed_pages > capacity_pages ? committed_pages - capacity_pages : 0;
    if (requested_pages == 0) return excess_pages;

    const auto free_pages = committed_pages < capacity_pages ? capacity_pages - committed_pages : 0;
    if (free_pages >= requested_pages) return excess_pages;
    return std::max(excess_pages, requested_pages);
}

uint64_t bounded_subtract(uint64_t value, uint64_t decrement) { return decrement >= value ? 0 : value - decrement; }

HiCacheOperationHeader make_operation_header(HiCacheOperationKind kind, const std::string & operation_id, const std::string & cache_scope,
                                             const std::string & request_key, const std::string & owner, HiCacheNodeId anchor_node,
                                             const std::vector<HiCacheNodeId> & node_ids, const std::vector<std::string> & pages, uint64_t enqueue_ts,
                                             uint64_t enqueue_epoch) {
    return HiCacheOperationHeader{
        .operation_id = operation_id,
        .kind = kind,
        .cache_scope = cache_scope,
        .request_key = request_key,
        .owner = owner,
        .anchor_node = anchor_node,
        .node_ids = node_ids,
        .pages = pages,
        .enqueue_epoch = enqueue_epoch,
        .enqueue_ts = enqueue_ts,
    };
}

std::string request_ref_owner(const std::string & request_key) { return request_key.empty() ? std::string{} : request_key + ":request"; }

std::string storage_hash_from_fact_value(const std::string & value) {
    const auto delimiter = value.find('|');
    if (delimiter == std::string::npos) return value;
    return value.substr(delimiter + 1);
}

} // namespace

void HiCacheState::DeviceAllocatorLedger::configure(uint64_t pages, bool sort_required) {
    if (initialized && capacity_pages == pages && need_sort == sort_required) return;
    initialized = true;
    need_sort = sort_required;
    capacity_pages = pages;
    free_pages = pages;
    release_pages = 0;
}

uint64_t HiCacheState::DeviceAllocatorLedger::available_pages() const { return free_pages + release_pages; }

bool HiCacheState::DeviceAllocatorLedger::should_evict(uint64_t requested_pages) const {
    return initialized && capacity_pages > 0 && requested_pages > 0 && available_pages() < requested_pages;
}

void HiCacheState::DeviceAllocatorLedger::merge_release_pages() {
    free_pages += release_pages;
    release_pages = 0;
}

void HiCacheState::DeviceAllocatorLedger::merge_before_extend(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size) {
    if (!need_sort || page_size == 0) return;
    const auto needed_pages = page_size == 1 ? extend_tokens : extend_tokens / page_size + batch_size + 1;
    if (needed_pages > free_pages) merge_release_pages();
}

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

uint64_t HiCacheState::DeviceAllocatorLedger::release(uint64_t pages) {
    if (pages == 0 || capacity_pages == 0) return 0;
    const auto room = capacity_pages > available_pages() ? capacity_pages - available_pages() : 0;
    const auto released = std::min(pages, room);
    if (need_sort) release_pages += released;
    else free_pages += released;
    return released;
}

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)), pager_(config_), policy_(config_) {}

std::string HiCacheState::normalized_scope(const HiCacheFact & fact) const { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

std::string HiCacheState::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

HiCacheState::ScopedState & HiCacheState::scope_state(const HiCacheFact & fact) { return scopes_[normalized_scope(fact)]; }

void HiCacheState::ensure_device_allocator(ScopedState & scope) {
    scope.device_allocator.configure(policy_.l1_capacity_pages(), policy_.resolved().device_allocator_need_sort);
}

bool HiCacheState::inserted_device_dirty_visible_at_insert_boundary() const {
    if (policy_.write_back_enabled()) return true;
    return policy_.write_count_enabled() && policy_.write_through_threshold() > 1;
}

HiCacheState::PrefetchIoProgressEstimate HiCacheState::estimate_prefetch_io_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact) const {
    (void)op;
    (void)fact;
    return PrefetchIoProgressEstimate{
        .completed_pages = {},
        .model_name = "zero_progress",
        .reason = "zero-progress IO model: invariant input has no calibrated storage transfer progress",
    };
}

HiCacheState::PrefetchProgressEstimate HiCacheState::estimate_prefetch_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact,
                                                                                bool require_full_completion) const {
    auto estimate = PrefetchProgressEstimate{
        .storage_hit_pages = static_cast<uint64_t>(op.hit_pages.size()),
        .storage_hit_sufficient = op.hit_pages.size() >= policy_.prefetch_threshold_pages(),
        .terminal_checkpoint = policy_.terminal_prefetch_checkpoint(fact.check_kind),
        .timeout_elapsed = policy_.prefetch_timeout_elapsed(op.header.enqueue_ts, fact.ts, static_cast<uint64_t>(op.planned_pages.size() * config_.page_size)),
    };
    if (!estimate.storage_hit_sufficient) {
        estimate.reason = "storage hit prefix is below target prefetch threshold";
        return estimate;
    }

    const auto policy = policy_.prefetch_policy();
    if (require_full_completion) {
        estimate.completed_pages = op.hit_pages;
        estimate.fully_completed = estimate.completed_pages.size() == op.hit_pages.size();
        estimate.reason = "stop boundary requires completed IO; modeled completed prefix equals storage hit prefix";
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
        estimate.fully_completed = estimate.completed_pages.size() == op.hit_pages.size();
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

void HiCacheState::drain_write_through_backup_refs(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                   ScopedState & scope, const std::string & reason) {
    if (scope.pending_write_through_backups.empty()) return;

    auto pending = std::exchange(scope.pending_write_through_backups, {});
    for (const auto & backup : pending) {
        const auto before = digest();
        const auto ref = scope.refs.release_owner(scope.tree, backup.owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, reason);
        record_transition(fact, summary, transitions, "complete_write_through_backup", "writeback", backup.pages, before);
    }
}

void HiCacheState::record_token_resolution(const HiCacheFact & fact, HiCacheSummary & summary, const HiCacheTokenResolution & resolution) const {
    const auto status = hicache_token_resolution_status_name(resolution.status);
    summary.token_resolution_by_status[status]++;
    summary.token_path_diagnostics[fact.role + "." + status]++;

    if (!resolution.ok()) {
        summary.missing_invariant_facts["token_resolution_" + status]++;
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

void HiCacheState::sync_capacity_for_ref(ScopedState & scope, const std::string & cache_scope, const HiCacheRefMutation & mutation,
                                         const std::string & reason) {
    std::set<HiCacheNodeId> nodes;
    nodes.insert(mutation.lock_nodes.begin(), mutation.lock_nodes.end());
    nodes.insert(mutation.host_nodes.begin(), mutation.host_nodes.end());
    sync_capacity(scope, cache_scope, { nodes.begin(), nodes.end() }, reason);
}

void HiCacheState::record_policy_decision(const HiCacheFact & fact, HiCachePolicyDecisionRecord decision) {
    decision.decision_epoch = ++policy_decision_epoch_;
    decision.cache_scope = normalized_scope(fact);
    decision.request_key = scoped_request_key(fact);
    decision.role = fact.role;
    decision.event_name = fact.event_name;
    policy_decisions_.push_back(std::move(decision));
}

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
    case HiCacheFactRole::StorageBackendReadable:
        apply_storage_backend_readable(fact, summary, transitions);
        break;
    case HiCacheFactRole::Unknown:
        break;
    }
    return transitions;
}

void HiCacheState::update_request_state(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages) {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto lookup = scope.tree.lookup_peek(pages);
    auto & request = scope.requests[key];
    request.request_key = key;
    request.cache_scope = normalized_scope(fact);
    request.full_pages = pages;
    request.device_pages = lookup.device_pages;
    request.host_pages = lookup.host_pages;
    request.device_chain = lookup.device_chain;
    request.host_chain = lookup.host_chain;
}

void HiCacheState::apply_request_bound_match_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto resolution = token_directory_.resolve_match_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;

    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
    resolve_prefetch_before_request_use(fact, summary, transitions, scope);
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "match_anchor_touch");

    const auto request_key = scoped_request_key(fact);
    /**
     * @brief modeled loadback：只把 host-visible prefix 同步 materialize 到 L1。
     *
     * SGLang 的 request match 命中 host cache 时会在 admission 前把 host KV
     * load 回 device，并把这段 prefix 放入 request 的 `prefix_indices`。storage
     * readable 只说明 L3 可读，不等价于本轮已经完成 H2D loadback，因此这里不能
     * 使用 `visible_pages`；必须要求 radix 上已经有 host-visible residency。
     *
     * 当前 normal invariant 还没有 scheduler `host_hit_length`、`mem_quota` 或 loadback
     * intent。由于 write-back ACK 被折叠为同步，model 可能比真实 SGLang 更早看到
     * host-visible prefix；因此 loadback 只能 opportunistic 消费当前 free pages，不能
     * 由这个推导结果主动触发 device eviction。
     */
    const auto promotable_pages = lookup.host_pages;
    if (promotable_pages.size() > lookup.device_pages.size()) {
        const auto loadback_pages = static_cast<uint64_t>(promotable_pages.size() - lookup.device_pages.size());
        scope.device_allocator.merge_before_page_allocation(loadback_pages);
        if (!scope.device_allocator.can_allocate(loadback_pages)) {
            record_policy_decision(
                fact,
                HiCachePolicyDecisionRecord{
                    .policy_area = "device_allocator",
                    .policy_name = "loadback_allocation",
                    .decision = "skip_loadback_eviction_without_intent",
                    .reason = "loadback requires scheduler host_hit/loadback intent; modeled host visibility must not invent device eviction",
                    .accepted = false,
                    .requested_pages = loadback_pages,
                    .capacity_pages = scope.device_allocator.capacity_pages,
                    .allocator_free_pages = scope.device_allocator.free_pages,
                    .allocator_release_pages = scope.device_allocator.release_pages,
                    .allocator_available_pages = scope.device_allocator.available_pages(),
                    .pages = promotable_pages,
                });
            update_request_state(fact, scope, pages);
            return;
        }
        const auto consumed_pages = scope.device_allocator.allocate(loadback_pages);
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "device_allocator",
                                   .policy_name = "loadback_allocation",
                                   .decision = consumed_pages == loadback_pages ? "consume_device_pages_for_loadback" : "skip_loadback_allocation_oom",
                                   .reason = "sglang load_back allocates device pages before host prefix becomes L1 resident",
                                   .accepted = consumed_pages == loadback_pages,
                                   .requested_pages = loadback_pages,
                                   .allocated_pages = consumed_pages,
                                   .capacity_pages = scope.device_allocator.capacity_pages,
                                   .allocator_free_pages = scope.device_allocator.free_pages,
                                   .allocator_release_pages = scope.device_allocator.release_pages,
                                   .allocator_available_pages = scope.device_allocator.available_pages(),
                                   .allocator_consumed_pages = consumed_pages,
                                   .pages = promotable_pages,
                               });
        if (consumed_pages != loadback_pages) {
            update_request_state(fact, scope, pages);
            return;
        }

        const auto loadback_id = scope.clock.next_operation_id("loadback");
        const auto loadback_owner = request_key + ":loadback:" + loadback_id;
        const auto before_enqueue = digest();
        scope.async_ops.upsert_loadback(HiCacheLoadbackOperation{
            .header = make_operation_header(HiCacheOperationKind::Loadback,
                                            loadback_id,
                                            normalized_scope(fact),
                                            request_key,
                                            loadback_owner,
                                            lookup.terminal_node,
                                            lookup.topology_chain,
                                            promotable_pages,
                                            fact.ts,
                                            0),
            .target_node = lookup.terminal_node,
        });
        auto ref = scope.refs.acquire_lock(scope.tree, loadback_owner, "loadback", request_key, loadback_id, lookup.topology_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "loadback_ref_acquire");
        record_transition(fact, summary, transitions, "enqueue_loadback", "loadback", promotable_pages, before_enqueue);
        const auto before = digest();
        auto insert = scope.tree.insert_device_path(promotable_pages, fact.priority, false);
        sync_capacity_for_insert(scope, normalized_scope(fact), insert, "loadback_insert_device");
        record_transition(fact,
                          summary,
                          transitions,
                          "promote_visible_prefix_to_l1",
                          "L1",
                          flatten_node_pages(scope.tree, insert.restored_device_nodes),
                          before);
        const auto before_complete = digest();
        scope.async_ops.set_loadback_state(loadback_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
        ref = scope.refs.release_owner(scope.tree, loadback_owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "loadback_ref_release");
        record_transition(fact, summary, transitions, "complete_loadback", "loadback", promotable_pages, before_complete);
    }

    update_request_state(fact, scope, pages);
}

void HiCacheState::apply_request_admission(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto resolution = token_directory_.resolve_admission_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto admission_token_count = resolution.ok() ? resolution.token_count : fact.token_count;
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
    resolve_prefetch_before_request_use(fact, summary, transitions, scope);
    update_request_state(fact, scope, pages);

    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto & request = scope.requests[key];
    const auto owner = request_ref_owner(key);
    auto ref = scope.refs.release_owner(scope.tree, owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_ref_release_before_admission");
    ref = scope.refs.acquire_lock(scope.tree, owner, "request", key, "", request.device_chain);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_lock_ref_acquire");
    ref = scope.refs.acquire_host(scope.tree, owner, "request", key, "", request.host_chain);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_host_ref_acquire");
    const auto full_missing_pages =
        request.full_pages.size() > request.device_pages.size() ? static_cast<uint64_t>(request.full_pages.size() - request.device_pages.size()) : 0;
    const auto device_prefix_pages = static_cast<uint64_t>(request.device_pages.size());
    const auto device_prefix_tokens = device_prefix_pages * page_path.page_size;
    const auto prior_committed_prefix_tokens =
        request.lifecycle_state == "unfinished" ? std::min(request.committed_tokens, admission_token_count) : uint64_t{ 0 };
    const auto allocation_prefix_tokens = std::max(device_prefix_tokens, prior_committed_prefix_tokens);
    const auto allocation_intent =
        make_extend_allocation_intent(admission_token_count, allocation_prefix_tokens, page_path.page_size, policy_.extend_allocation_batch_size());
    const auto allocation_pressure_needed = allocation_intent.needs_pressure();
    const auto allocation_decision = !allocation_pressure_needed ? "skip_full_hit_allocation_pressure"
                                     : full_missing_pages > 0    ? "reserve_target_extend_budget"
                                                                 : "reserve_partial_tail_budget";
    const auto allocation_reason = !allocation_pressure_needed ? "target radix covers complete pages and request has no partial tail"
                                   : full_missing_pages > 0
                                       ? "explicit batch_size=1 allocation intent uses target device prefix to derive SGLang extend_num_tokens"
                                       : "complete target pages hit but partial tail still requires explicit batch_size=1 allocator pressure";
    const auto allocator_available_before = scope.device_allocator.available_pages();
    enforce_device_capacity(fact, summary, transitions, scope, allocation_intent.requested_pages);
    scope.device_allocator.merge_before_extend(allocation_intent.extend_tokens, allocation_intent.batch_size, page_path.page_size);
    const auto consumed_pages = scope.device_allocator.allocate(allocation_intent.allocated_pages);
    request.kv_allocated_pages += consumed_pages;
    request.cache_protected_pages = std::max(request.cache_protected_pages, device_prefix_pages);
    request.page_aligned_key_pages = static_cast<uint64_t>(pages.size());
    request.active_request_pages = request.kv_allocated_pages;
    request.lifecycle_state = "admitted";
    const auto capacity_snapshot = scope.capacity.snapshot();
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "device_allocation",
                               .policy_name = "extend_allocation_intent",
                               .decision = allocation_decision,
                               .reason = allocation_reason,
                               .accepted = allocation_pressure_needed,
                               .requested_pages = allocation_intent.requested_pages,
                               .requested_tokens = allocation_intent.requested_tokens,
                               .candidate_pages = full_missing_pages,
                               .hit_pages = device_prefix_pages,
                               .batch_size = allocation_intent.batch_size,
                               .extend_tokens = allocation_intent.extend_tokens,
                               .allocated_pages = consumed_pages,
                               .capacity_pages = policy_.l1_capacity_pages(),
                               .occupied_pages = capacity_snapshot.occupied_device_pages,
                               .reserved_pages = request.active_request_pages,
                               .allocator_free_pages = scope.device_allocator.free_pages,
                               .allocator_release_pages = scope.device_allocator.release_pages,
                               .allocator_available_pages = scope.device_allocator.available_pages(),
                               .allocator_available_before_pages = allocator_available_before,
                               .allocator_consumed_pages = consumed_pages,
                               .pages = request.full_pages,
                           });
    record_transition(fact, summary, transitions, "acquire_request_ref", "node_ref", request.device_pages, digest());
    drain_deferred_host_releases(fact, scope);
}

HiCacheInsertResult HiCacheState::insert_request_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                      ScopedState & scope, const std::vector<std::string> & pages) {
    const auto before = digest();
    const auto dirty_visible_at_insert = inserted_device_dirty_visible_at_insert_boundary();
    auto insert = scope.tree.insert_device_path(pages, fact.priority, dirty_visible_at_insert);
    sync_capacity_for_insert(scope, normalized_scope(fact), insert, "request_insert_device");
    auto new_pages = flatten_node_pages(scope.tree, insert.new_device_nodes);
    auto restored_pages = flatten_node_pages(scope.tree, insert.restored_device_nodes);
    auto dirtied_pages = flatten_node_pages(scope.tree, insert.dirtied_device_nodes);
    record_transition(fact, summary, transitions, "add_l1_residency", "L1", new_pages, before);
    record_transition(fact, summary, transitions, "restore_l1_residency", "L1", restored_pages, before);
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "write_policy",
                               .policy_name = policy_.write_policy(),
                               .decision = dirty_visible_at_insert ? "mark_inserted_device_pages_dirty" : "defer_to_immediate_hit_count_backup",
                               .reason = dirty_visible_at_insert ? "inserted or recomputed unbacked device pages remain dirty beyond the insert boundary"
                                                                 : "hit-count backup reaches the threshold inside the same insert boundary",
                               .accepted = dirty_visible_at_insert,
                               .candidate_pages = static_cast<uint64_t>(dirtied_pages.size()),
                               .threshold_pages = policy_.write_through_threshold(),
                               .pages = dirtied_pages,
                           });
    if (dirty_visible_at_insert) record_transition(fact, summary, transitions, "mark_dirty", "dirty", dirtied_pages, before);
    return insert;
}

void HiCacheState::apply_request_lifecycle_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto kind = lower_copy(fact.lifecycle_kind);
    if (!kind.empty() && kind != "finished" && kind != "unfinished") return;

    const auto resolution = token_directory_.resolve_lifecycle_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto lifecycle_token_count = resolution.token_count;
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto & request = scope.requests[key];
    const auto protected_pages_before_insert = request.cache_protected_pages;
    const auto request_owned_pages_before_insert = request.kv_allocated_pages;
    const auto insert = insert_request_path(fact, summary, transitions, scope, pages);
    apply_write_count_policy(fact, summary, transitions, scope, pages);
    update_request_state(fact, scope, pages);

    auto it = scope.requests.find(key);
    if (it == scope.requests.end()) return;
    const auto duplicate_pages = std::min(bounded_subtract(insert.existing_device_prefix_pages, protected_pages_before_insert), it->second.kv_allocated_pages);
    const auto owned_after_duplicate = bounded_subtract(it->second.kv_allocated_pages, duplicate_pages);
    const auto total_committed_pages = ceil_div(lifecycle_token_count, page_path.page_size);
    const auto page_aligned_pages = static_cast<uint64_t>(pages.size());
    const auto tail_pages = bounded_subtract(total_committed_pages, page_aligned_pages);
    const auto unfinished_tail_pages = std::min(tail_pages, owned_after_duplicate);
    const auto tail_release_pages = kind == "finished" || kind.empty() ? unfinished_tail_pages : uint64_t{ 0 };
    const auto released_pages = scope.device_allocator.release(duplicate_pages + tail_release_pages);
    it->second.kv_allocated_pages = kind == "unfinished" ? unfinished_tail_pages : uint64_t{ 0 };
    it->second.committed_tokens = lifecycle_token_count;
    it->second.page_aligned_key_pages = page_aligned_pages;
    it->second.active_request_pages = it->second.kv_allocated_pages;
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "request_lifecycle",
                               .policy_name = "device_allocator_release",
                               .decision = kind == "unfinished" ? "release_duplicate_keep_tail" : "release_duplicate_and_tail",
                               .reason = "sglang request cache lifecycle frees duplicate radix-covered KV and finished-request tail KV",
                               .accepted = released_pages > 0,
                               .candidate_pages = request_owned_pages_before_insert,
                               .hit_pages = protected_pages_before_insert,
                               .allocated_pages = it->second.kv_allocated_pages,
                               .capacity_pages = scope.device_allocator.capacity_pages,
                               .allocator_free_pages = scope.device_allocator.free_pages,
                               .allocator_release_pages = scope.device_allocator.release_pages,
                               .allocator_available_pages = scope.device_allocator.available_pages(),
                               .allocator_released_pages = released_pages,
                               .lifecycle_duplicate_pages = duplicate_pages,
                               .lifecycle_tail_pages = tail_release_pages,
                               .pages = pages,
                           });
    if (kind == "unfinished") {
        it->second.lifecycle_state = "unfinished";
        it->second.cache_protected_pages = page_aligned_pages;
        it->second.active_request_pages = it->second.kv_allocated_pages;
        const auto owner = request_ref_owner(key);
        auto ref = scope.refs.release_owner(scope.tree, owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_ref_release_before_unfinished");
        ref = scope.refs.acquire_lock(scope.tree, owner, "request", key, "", it->second.device_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_lock_ref_acquire_unfinished");
        ref = scope.refs.acquire_host(scope.tree, owner, "request", key, "", it->second.host_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_host_ref_acquire_unfinished");
    }
    else {
        const auto before = digest();
        const auto ref = scope.refs.release_owner(scope.tree, request_ref_owner(key));
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_ref_release_finished");
        record_transition(fact, summary, transitions, "release_request_ref", "node_ref", it->second.full_pages, before);
        scope.requests.erase(it);
    }
}

bool HiCacheState::commit_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      ScopedState & scope, HiCacheNodeId node_id, bool storage_readable) {
    const auto pages = scope.tree.node_pages(node_id);
    const auto * node = scope.tree.node(node_id);
    if (node == nullptr || pages.empty()) return false;
    const auto host_allocation_pages = !(node->residency.host_present && node->residency.host_visible) ? static_cast<uint64_t>(pages.size()) : uint64_t{ 0 };
    if (host_allocation_pages > 0) {
        const auto allocation = request_host_allocation(fact, summary, transitions, scope, host_allocation_pages, host_allocation_pages, false, "write_backup");
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "host_allocation",
                                   .policy_name = "write_backup",
                                   .decision = allocation.accepted ? "accept_host_backup_pages" : "skip_host_backup_capacity",
                                   .reason = allocation.accepted ? "target host pool can fit write_backup allocation after SGLang-style cleanup"
                                                                 : "target host pool still lacks space after SGLang-style cleanup",
                                   .accepted = allocation.accepted,
                                   .requested_pages = host_allocation_pages,
                                   .candidate_pages = allocation.accepted_pages,
                                   .capacity_pages = allocation.capacity_pages,
                                   .occupied_pages = allocation.occupied_pages,
                                   .reserved_pages = allocation.reserved_pages,
                                   .pages = pages,
                               });
        if (!allocation.accepted) return false;
    }

    std::string storage_id;
    if (storage_readable) {
        storage_id = scope.clock.next_operation_id("storage");
        const auto storage_owner = scoped_request_key(fact) + ":storage:" + storage_id;
        const auto before_enqueue = digest();
        scope.async_ops.upsert_storage(HiCacheStorageOperation{
            .header = make_operation_header(HiCacheOperationKind::Storage,
                                            storage_id,
                                            normalized_scope(fact),
                                            scoped_request_key(fact),
                                            storage_owner,
                                            node_id,
                                            std::vector<HiCacheNodeId>{ node_id },
                                            pages,
                                            fact.ts,
                                            0),
            .node_id = node_id,
        });
        const auto ref =
            scope.refs.acquire_host(scope.tree, storage_owner, "storage", scoped_request_key(fact), storage_id, std::vector<HiCacheNodeId>{ node_id });
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_acquire");
        record_transition(fact, summary, transitions, "enqueue_storage_backup", "storage", pages, before_enqueue);
    }
    const auto before = digest();
    scope.tree.mark_host_visible(node_id, storage_readable);
    scope.tree.clear_dirty(node_id);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "commit_host_backup");
    if (storage_readable) {
        scope.storage.mark_readable_pages(normalized_scope(fact), pages);
        scope.storage.mark_materialized_pages(pages, node_id);
    }
    record_transition(fact, summary, transitions, storage_readable ? "commit_host_storage_backup" : "commit_host_backup", "L2", pages, before);
    if (storage_readable && policy_.write_count_enabled()) { hold_write_through_backup_ref(fact, summary, transitions, scope, node_id, pages); }
    if (!storage_id.empty()) {
        const auto before_complete = digest();
        scope.async_ops.set_storage_state(storage_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
        const auto ref = scope.refs.release_owner(scope.tree, scoped_request_key(fact) + ":storage:" + storage_id);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_release");
        record_transition(fact, summary, transitions, "complete_storage_backup", "storage", pages, before_complete);
    }
    return true;
}

void HiCacheState::hold_write_through_backup_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                 ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages) {
    const auto chain = scope.tree.ancestor_node_ids(node_id);
    if (chain.empty()) return;

    const auto operation_id = scope.clock.next_operation_id("write_through_backup");
    const auto owner = scoped_request_key(fact) + ":write_through_backup:" + operation_id;
    const auto before = digest();
    const auto ref = scope.refs.acquire_lock(scope.tree, owner, "write_through_backup", scoped_request_key(fact), operation_id, chain);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "write_through_backup_ref_acquire");
    scope.pending_write_through_backups.push_back(PendingWriteThroughBackup{
        .owner = owner,
        .pages = pages,
    });
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .operation_id = operation_id,
                               .policy_area = "write_policy",
                               .policy_name = policy_.write_policy(),
                               .decision = "hold_write_through_backup_ref",
                               .reason = "SGLang write_backup() protects non-write-back nodes until writing_check() observes the async CPU write ack",
                               .accepted = true,
                               .candidate_pages = static_cast<uint64_t>(pages.size()),
                               .pages = pages,
                           });
    record_transition(fact, summary, transitions, "enqueue_write_through_backup", "writeback", pages, before);
}

void HiCacheState::apply_write_count_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            ScopedState & scope, const std::vector<std::string> & pages) {
    if (!policy_.write_count_enabled()) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "write_policy",
                                   .policy_name = policy_.write_policy(),
                                   .decision = "skip_hit_count_backup",
                                   .reason = "target write policy does not use hit-count backup",
                                   .accepted = false,
                                   .candidate_pages = static_cast<uint64_t>(pages.size()),
                                   .pages = pages,
                               });
        return;
    }
    const auto threshold = policy_.write_through_threshold();
    if (threshold == 0) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "write_policy",
                                   .policy_name = policy_.write_policy(),
                                   .decision = "skip_hit_count_backup",
                                   .reason = "resolved write-through threshold is zero",
                                   .accepted = false,
                                   .candidate_pages = static_cast<uint64_t>(pages.size()),
                                   .pages = pages,
                               });
        return;
    }

    auto lookup = scope.tree.lookup_peek(pages);
    for (const auto node_id : lookup.topology_chain) {
        auto * node = scope.tree.mutable_node(node_id);
        if (node == nullptr || !node->residency.device_present) continue;
        const auto before = digest();
        node->hit_count++;
        record_transition(fact, summary, transitions, "increment_hit_count", "hit_count", node->pages, before);
        const auto should_backup = !has_host_backup(*node) && node->hit_count >= threshold;
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "write_policy",
                                   .policy_name = policy_.write_policy(),
                                   .decision = should_backup ? "commit_hit_count_backup" : "wait_for_hit_count_backup",
                                   .reason = has_host_backup(*node) ? "node already has host backup"
                                             : should_backup        ? "node hit count reached write-through threshold"
                                                                    : "node hit count is below write-through threshold",
                                   .accepted = should_backup,
                                   .candidate_pages = static_cast<uint64_t>(node->pages.size()),
                                   .hit_count = node->hit_count,
                                   .threshold_pages = threshold,
                                   .pages = node->pages,
                               });
        if (should_backup) (void)commit_host_backup(fact, summary, transitions, scope, node_id, true);
    }
}

uint64_t HiCacheState::evict_device_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         ScopedState & scope, HiCacheNodeId node_id) {
    auto * node = scope.tree.mutable_node(node_id);
    if (node == nullptr || !node->residency.device_present) return 0;

    const auto pages = node->pages;
    const auto released_pages = static_cast<uint64_t>(pages.size());
    const bool needs_writeback = policy_.write_back_enabled() && node->residency.device_dirty;
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "write_policy",
                               .policy_name = policy_.write_policy(),
                               .decision = needs_writeback ? "enqueue_dirty_eviction_writeback" : "evict_without_writeback",
                               .reason = needs_writeback                ? "write_back dirty device node must refresh host backup before eviction"
                                         : node->residency.device_dirty ? "target write policy does not require dirty eviction writeback"
                                                                        : "device node is clean at eviction boundary",
                               .accepted = needs_writeback,
                               .candidate_pages = static_cast<uint64_t>(pages.size()),
                               .pages = pages,
                           });
    if (needs_writeback) {
        summary.dirty_eviction_events++;
        const auto writeback_id = scope.clock.next_operation_id("writeback");
        const auto writeback_owner = scoped_request_key(fact) + ":writeback:" + writeback_id;
        const auto before_enqueue = digest();
        scope.async_ops.upsert_writeback(HiCacheWritebackOperation{
            .header = make_operation_header(HiCacheOperationKind::Writeback,
                                            writeback_id,
                                            normalized_scope(fact),
                                            scoped_request_key(fact),
                                            writeback_owner,
                                            node_id,
                                            std::vector<HiCacheNodeId>{ node_id },
                                            pages,
                                            fact.ts,
                                            0),
            .node_id = node_id,
        });
        auto ref =
            scope.refs.acquire_lock(scope.tree, writeback_owner, "writeback", scoped_request_key(fact), writeback_id, std::vector<HiCacheNodeId>{ node_id });
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "writeback_ref_acquire");
        record_transition(fact, summary, transitions, "enqueue_writeback", "writeback", pages, before_enqueue);
        const auto committed = commit_host_backup(fact, summary, transitions, scope, node_id, true);
        const auto before_complete = digest();
        scope.async_ops.set_writeback_state(writeback_id,
                                            committed ? HiCacheOperationState::Committed : HiCacheOperationState::Cancelled,
                                            committed ? "sync_commit" : "host_backup_capacity_rejected",
                                            fact.ts);
        ref = scope.refs.release_owner(scope.tree, writeback_owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "writeback_ref_release");
        record_transition(fact, summary, transitions, committed ? "complete_writeback" : "cancel_writeback", "writeback", pages, before_complete);
        if (!committed) return 0;
    }

    const auto before = digest();
    if (has_host_backup(*node)) scope.tree.demote_device_to_host(node_id, false);
    else scope.tree.remove_device_regular(node_id);
    const auto allocator_released_pages = scope.device_allocator.release(released_pages);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "evict_device_node");
    record_transition(fact, summary, transitions, "evict_l1_node", "L1", pages, before);
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "device_allocator",
                               .policy_name = "device_eviction_free",
                               .decision = "release_device_pages_to_allocator",
                               .reason = "sglang device eviction frees the victim node value back to token_to_kv_pool_allocator",
                               .accepted = allocator_released_pages > 0,
                               .candidate_pages = released_pages,
                               .capacity_pages = scope.device_allocator.capacity_pages,
                               .allocator_free_pages = scope.device_allocator.free_pages,
                               .allocator_release_pages = scope.device_allocator.release_pages,
                               .allocator_available_pages = scope.device_allocator.available_pages(),
                               .allocator_released_pages = allocator_released_pages,
                               .pages = pages,
                           });
    return released_pages;
}

uint64_t HiCacheState::evict_host_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                       ScopedState & scope, HiCacheNodeId node_id) {
    const auto * node = scope.tree.node(node_id);
    const auto pages = node == nullptr ? std::vector<std::string>{} : node->pages;
    if (node != nullptr && node->refs.host_ref_total > 0) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "host_cleanup",
                                   .policy_name = "evict_host",
                                   .decision = "skip_host_ref_protected_leaf",
                                   .reason = "SGLang evict_host skips a popped host leaf while host_ref_counter is positive",
                                   .accepted = false,
                                   .candidate_pages = static_cast<uint64_t>(pages.size()),
                                   .pages = pages,
                               });
        sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "evict_host_ref_protected");
        return 0;
    }

    const auto before = digest();
    const auto result = scope.tree.evict_host_leaf(node_id);
    sync_capacity(scope,
                  normalized_scope(fact),
                  result.affected_nodes.empty() ? std::vector<HiCacheNodeId>{ node_id } : result.affected_nodes,
                  result.evicted ? "evict_host_leaf" : "evict_host_leaf_skipped");
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "host_cleanup",
                               .policy_name = "evict_host",
                               .decision = result.evicted ? "evict_host_leaf" : "skip_host_leaf",
                               .reason = result.reason,
                               .accepted = result.evicted,
                               .candidate_pages = static_cast<uint64_t>(result.pages.size()),
                               .pages = result.pages,
                           });
    if (!result.evicted) return 0;
    record_transition(fact, summary, transitions, "evict_host_node", "L2", pages, before);
    return static_cast<uint64_t>(pages.size());
}

void HiCacheState::enforce_device_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                           ScopedState & scope, uint64_t requested_pages) {
    ensure_device_allocator(scope);
    const auto capacity = scope.device_allocator.capacity_pages;
    if (capacity == 0 || requested_pages == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, "device_allocator_budget");
    if (!scope.device_allocator.should_evict(requested_pages)) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "device_allocator",
                                   .policy_name = "available_size_gate",
                                   .decision = "skip_device_eviction",
                                   .reason = "sglang standard allocator evicts only when available_size is below request budget",
                                   .accepted = false,
                                   .requested_pages = requested_pages,
                                   .capacity_pages = capacity,
                                   .allocator_free_pages = scope.device_allocator.free_pages,
                                   .allocator_release_pages = scope.device_allocator.release_pages,
                                   .allocator_available_pages = scope.device_allocator.available_pages(),
                               });
        return;
    }
    auto target = requested_pages;
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "device_allocator",
                               .policy_name = "available_size_gate",
                               .decision = "evict_for_device_allocation",
                               .reason = "sglang standard allocator passes the full request budget to tree_cache.evict once available_size is insufficient",
                               .accepted = true,
                               .requested_pages = requested_pages,
                               .capacity_pages = capacity,
                               .allocator_free_pages = scope.device_allocator.free_pages,
                               .allocator_release_pages = scope.device_allocator.release_pages,
                               .allocator_available_pages = scope.device_allocator.available_pages(),
                           });
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {}, "device_allocator_loop");
        const auto victim = scope.capacity.select_device_victim(capacity, requested_pages, "device_allocator_loop");
        if (!victim) break;
        const auto released_pages = evict_device_node(fact, summary, transitions, scope, *victim);
        if (released_pages == 0 || released_pages >= target) break;
        target -= released_pages;
    }
}

void HiCacheState::enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         ScopedState & scope, uint64_t requested_pages) {
    const auto capacity = policy_.l2_capacity_pages();
    if (capacity == 0) return;
    const auto budget_reason = requested_pages > 0 ? "host_allocation_request_budget" : "host_capacity_budget";
    const auto loop_reason = requested_pages > 0 ? "host_allocation_request_loop" : "host_capacity_loop";
    sync_capacity(scope, normalized_scope(fact), {}, budget_reason);
    const auto snapshot = scope.capacity.snapshot();
    auto target = allocation_cleanup_target(snapshot.occupied_host_pages, snapshot.reserved_host_pages, capacity, requested_pages);
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {}, loop_reason);
        const auto victim = scope.capacity.select_host_victim(capacity, requested_pages, loop_reason);
        if (!victim) break;
        const auto victim_pages = evict_host_node(fact, summary, transitions, scope, *victim);
        if (victim_pages == 0) break;
        if (victim_pages >= target) break;
        target -= victim_pages;
    }
}

void HiCacheState::drain_deferred_host_releases(const HiCacheFact & fact, ScopedState & scope) {
    const auto cache_scope = normalized_scope(fact);
    const auto released_pages = scope.async_ops.release_deferred_host_pages(cache_scope);
    if (released_pages == 0) return;

    sync_capacity(scope, cache_scope, {}, "prefetch_deferred_host_release_drain");
    record_policy_decision(
        fact,
        HiCachePolicyDecisionRecord{
            .policy_area = "host_allocation",
            .policy_name = "prefetch_host_release_queue",
            .decision = "drain_deferred_host_release_pages",
            .reason = "sglang frees revoked or unused prefetch host pages through the storage control release queue after allocation pressure",
            .accepted = true,
            .allocator_released_pages = released_pages,
        });
}

HiCacheState::HostAllocationResult HiCacheState::request_host_allocation(const HiCacheFact & fact, HiCacheSummary & summary,
                                                                         std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                                                                         uint64_t requested_pages, uint64_t minimum_pages, bool allow_truncate,
                                                                         const std::string & reason) {
    auto result = HostAllocationResult{
        .requested_pages = requested_pages,
        .capacity_pages = policy_.l2_capacity_pages(),
    };
    if (requested_pages == 0) {
        result.accepted = true;
        return result;
    }
    if (result.capacity_pages == 0) {
        result.accepted = true;
        result.accepted_pages = requested_pages;
        return result;
    }

    enforce_host_capacity(fact, summary, transitions, scope, requested_pages);
    sync_capacity(scope, normalized_scope(fact), {}, reason + "_host_allocation_post_cleanup");
    const auto snapshot = scope.capacity.snapshot();
    result.occupied_pages = snapshot.occupied_host_pages;
    result.reserved_pages = snapshot.reserved_host_pages;
    const auto committed_pages = snapshot.occupied_host_pages + snapshot.reserved_host_pages;
    const auto available_pages = committed_pages < result.capacity_pages ? result.capacity_pages - committed_pages : uint64_t{ 0 };
    if (available_pages >= requested_pages) {
        result.accepted = true;
        result.accepted_pages = requested_pages;
        return result;
    }
    if (allow_truncate && available_pages >= minimum_pages) {
        result.accepted = true;
        result.truncated = true;
        result.accepted_pages = available_pages;
    }
    return result;
}

void HiCacheState::apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto resolution = token_directory_.resolve_prefetch_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
    drain_deferred_host_releases(fact, scope);
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "prefetch_lookup_touch");
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
        .requested_host_pages = requested_pages,
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
                               .requested_pages = requested_pages,
                               .candidate_pages = static_cast<uint64_t>(planned_pages.size()),
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
    op.reserved_host_pages = bounded_subtract(op.reserved_host_pages, static_cast<uint64_t>(op.completed_pages.size()));
    const auto ref = scope.refs.release_owner(scope.tree, op.header.owner);
    sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_apply_pending_host_release");
    record_transition(fact, summary, transitions, "apply_prefetch_host_visibility", "L2", flatten_node_pages(scope.tree, insert.new_host_nodes), before_apply);
}

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

void HiCacheState::resolve_prefetch_before_request_use(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                       ScopedState & scope) {
    auto * op = scope.async_ops.prefetch_for_request(scoped_request_key(fact));
    if (op == nullptr || !prefetch_active(*op) || op->hit_pages.size() < policy_.prefetch_threshold_pages()) return;

    const auto policy = policy_.prefetch_policy();
    if (policy == "wait_complete") {
        auto progress = estimate_prefetch_progress(*op, fact, true);
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_request_boundary",
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
        drain_deferred_host_releases(fact, scope);
        return;
    }

    if (policy == "timeout") {
        const auto timeout_elapsed =
            policy_.prefetch_timeout_elapsed(op->header.enqueue_ts, fact.ts, static_cast<uint64_t>(op->planned_pages.size() * config_.page_size));
        auto progress = estimate_prefetch_progress(*op, fact, !timeout_elapsed);
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_request_boundary",
                                   .policy_name = policy,
                                   .decision = timeout_elapsed ? "cancel_prefetch_without_completed_io" : "apply_prefetch",
                                   .reason = progress.reason,
                                   .accepted = !timeout_elapsed,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .allocated_pages = static_cast<uint64_t>(progress.completed_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
        if (timeout_elapsed) {
            cancel_prefetch_pending_release(fact, summary, transitions, scope, *op, "prefetch_timeout_incomplete", HiCachePrefetchState::Late);
        }
        else {
            op->completed_pages = std::move(progress.completed_pages);
            apply_prefetch_ready(fact, summary, transitions, scope, *op);
            drain_deferred_host_releases(fact, scope);
        }
    }
}

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
    const auto checkpoint = scope.clock.record_target_checkpoint(normalized_scope(fact),
                                                                 request_key,
                                                                 fact.check_kind,
                                                                 fact.ts,
                                                                 policy_.terminal_prefetch_checkpoint(fact.check_kind),
                                                                 fact.source_event_index);
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
        const auto terminal = policy_.terminal_prefetch_checkpoint(fact.check_kind);
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
        auto progress = estimate_prefetch_progress(*op, fact, false);
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
        const auto terminal = policy_.terminal_prefetch_checkpoint(fact.check_kind);
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

void HiCacheState::apply_storage_backend_readable(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    std::vector<std::string> page_hashes;
    page_hashes.reserve(fact.storage_page_hashes.size());
    std::ranges::transform(fact.storage_page_hashes, std::back_inserter(page_hashes), storage_hash_from_fact_value);
    std::ranges::sort(page_hashes);
    page_hashes.erase(std::ranges::unique(page_hashes).begin(), page_hashes.end());
    if (page_hashes.empty()) return;

    auto & scope = scope_state(fact);
    const auto cache_scope = normalized_scope(fact);
    const auto before = digest();
    scope.storage.seed_readable_hashes(cache_scope, page_hashes, fact.storage_source.empty() ? "invariant_storage_backend_readable" : fact.storage_source);

    std::vector<std::string> page_ids;
    page_ids.reserve(page_hashes.size());
    std::ranges::transform(page_hashes, std::back_inserter(page_ids), [&](const auto & page_hash) { return pager_.scoped_page_id(cache_scope, page_hash); });
    record_transition(fact, summary, transitions, "seed_storage_backend_readable", "storage", page_ids, before);
}

std::vector<HiCacheStateTransition> HiCacheState::finalize(HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    HiCacheFact fact;
    fact.event_name = "hicache_finalize";
    fact.role = "prefetch_finalize";
    for (auto & [scope_name, scope] : scopes_) {
        fact.cache_scope = scope_name;
        const auto checkpoint = scope.clock.record_target_finalize_checkpoint(scope_name, fact.ts);
        for (auto & op : scope.async_ops.prefetch_ops() | std::views::values) {
            if (op.prefetch_state != HiCachePrefetchState::Pending && op.prefetch_state != HiCachePrefetchState::Ready) continue;
            op.header.checkpoint_epoch = checkpoint.checkpoint_epoch;
            op.header.checkpoint_ts = fact.ts;
            const auto before = digest();
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
            record_transition(fact, summary, transitions, "prefetch_suppressed", "prefetch", op.planned_pages, before);
        }
    }
    return transitions;
}

HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheSummary summary;
    summary.target_config = config;
    summary.resolved_policy = resolve_hicache_policy(config);

    HiCacheFactParser parser;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        parser.observe_token_dictionaries(event);
    }

    HiCacheState state(config);
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!parser.is_hicache_event(event)) continue;
        summary.input_hicache_events++;
        auto fact = parser.parse(node.id, event);
        summary.events_by_role[fact.role]++;

        auto route = route_hicache_fact(fact);
        if (!route.model_fact) {
            summary.skipped_non_invariant_events++;
            continue;
        }
        if (!route.known_role || !hicache_fact_role_implemented(route.role)) {
            summary.missing_invariant_facts["unknown_invariant_role"]++;
            continue;
        }
        const auto required_errors = hicache_required_fact_errors(fact, route.role, config.page_size > 0 ? config.page_size : fact.source_page_size);
        if (!required_errors.empty()) {
            std::ranges::for_each(required_errors, [&](const auto & error) { summary.missing_invariant_facts[error]++; });
            continue;
        }

        auto transitions = state.apply_fact(fact, route.role, summary);
        summary.processed_hicache_events++;
        summary.processed_events_by_role[hicache_fact_role_name(route.role)]++;
        summary.transition_trace.insert(summary.transition_trace.end(), transitions.begin(), transitions.end());
        summary.state_transition_count = summary.transition_trace.size();
    }

    auto final_transitions = state.finalize(summary);
    summary.transition_trace.insert(summary.transition_trace.end(), final_transitions.begin(), final_transitions.end());
    summary.state_transition_count = summary.transition_trace.size();

    const auto final_state = state.derived_state(HiCacheDerivedStateMode::MaterializedOnly);
    const auto inclusive_state = state.derived_state(HiCacheDerivedStateMode::StorageDirectoryInclusive);
    summary.final_state_derivation_mode = hicache_derived_state_mode_name(final_state.mode);
    summary.storage_directory_inclusive_state = inclusive_state;
    summary.active_ref_owner_count = state.active_ref_owner_count();
    summary.radix_split_count = state.radix_split_count();
    summary.radix_split_trace = state.radix_split_trace();
    summary.control_checkpoint_count = state.control_checkpoint_count();
    summary.control_checkpoint_trace = state.control_checkpoint_trace();
    summary.async_lifecycle_transition_count = state.async_lifecycle_transition_count();
    summary.async_lifecycle_trace = state.async_lifecycle_trace();
    summary.policy_decision_count = state.policy_decision_count();
    summary.policy_decision_trace = state.policy_decision_trace();
    summary.storage_known_page_count = state.storage_known_page_count();
    summary.storage_readable_page_count = state.storage_readable_page_count();
    summary.storage_backend_readable_count = state.storage_backend_readable_count();
    summary.storage_materialized_page_count = state.storage_materialized_page_count();
    summary.capacity_mutation_count = state.capacity_mutation_count();
    summary.capacity_victim_choice_count = state.capacity_victim_choice_count();
    summary.capacity_mutation_trace = state.capacity_mutation_trace();
    summary.capacity_victim_choices = state.capacity_victim_choices();
    summary.capacity_audit_issues = state.capacity_audit_issues();
    summary.capacity_audit_issue_count = summary.capacity_audit_issues.size();
    summary.ref_mutation_count = state.ref_mutation_count();
    summary.ref_mutation_trace = state.ref_mutation_trace();
    summary.ref_audit_issues = state.ref_audit_issues();
    summary.ref_audit_issue_count = summary.ref_audit_issues.size();
    summary.l1_resident_pages = hicache_sorted_vector(final_state.l1);
    summary.l2_resident_pages = hicache_sorted_vector(final_state.l2);
    summary.l3_resident_pages = hicache_sorted_vector(final_state.l3);
    summary.dirty_pages = hicache_sorted_vector(final_state.dirty);
    summary.backuped_pages = hicache_sorted_vector(final_state.backuped);
    summary.evicted_pages = hicache_sorted_vector(final_state.evicted);
    summary.locked_pages = hicache_sorted_vector(final_state.locked);
    summary.pending_writeback_pages = hicache_sorted_vector(final_state.pending_writeback);
    summary.prefetch_planned_pages = hicache_sorted_vector(final_state.prefetch_planned);
    summary.prefetch_ready_pages = hicache_sorted_vector(final_state.prefetch_ready);
    summary.prefetch_late_pages = hicache_sorted_vector(final_state.prefetch_late);
    summary.prefetch_suppressed_pages = hicache_sorted_vector(final_state.prefetch_suppressed);
    summary.page_hit_counts = final_state.page_hit_counts;
    if (summary.dirty_eviction_events > 0)
        summary.warnings.push_back("write_back eviction used synchronous modeled writeback; ack timing is intentionally not modeled yet.");
    if (summary.capacity_audit_issue_count > 0)
        summary.warnings.push_back("HiCache capacity index audit found mismatches between mutation-driven index and canonical tree.");
    if (summary.ref_audit_issue_count > 0) summary.warnings.push_back("HiCache ref ledger audit found mismatches between owner ledger and tree ref counters.");
    return summary;
}

} // namespace TraceGraph
