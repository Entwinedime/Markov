/**
 * @file
 * @brief Main execution path for the canonical-radix HiCache state model.
 */
#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <set>
#include <utility>

#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

/**
 * @brief Initializes the count-level projection of the device allocator.
 *
 * The ledger reconstructs free/release queue visibility but does not own page
 * identity. Page identity remains derived from the radix tree and capacity index.
 */
void HiCacheState::DeviceAllocatorLedger::configure(uint64_t pages, bool sort_required) {
    if (initialized && capacity_pages == pages && need_sort == sort_required) return;
    initialized = true;
    need_sort = sort_required;
    capacity_pages = pages;
    free_pages = pages;
    release_pages = 0;
}

uint64_t HiCacheState::DeviceAllocatorLedger::available_pages() const {
    return core::checked_add_u64(free_pages, release_pages, "HiCache device allocator availability exceeds uint64 range");
}

/** @brief Reports whether a page request triggers the SGLang eviction gate. */
bool HiCacheState::DeviceAllocatorLedger::should_evict(uint64_t requested_pages) const {
    return initialized && capacity_pages > 0 && requested_pages > 0 && available_pages() < requested_pages;
}

/** @brief Merges the pending release queue into allocator free pages. */
void HiCacheState::DeviceAllocatorLedger::merge_release_pages() {
    free_pages = core::checked_add_u64(free_pages, release_pages, "HiCache device allocator free page count exceeds uint64 range");
    release_pages = 0;
}

/**
 * @brief Reconstructs conditional release-queue synchronization before extend.
 *
 * The paged allocator includes batch overhead in its pressure gate. This count-level
 * projection preserves that timing so cleanup is not driven only by final occupancy.
 */
void HiCacheState::DeviceAllocatorLedger::merge_before_extend(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size) {
    if (!need_sort || page_size == 0) return;
    const auto needed_pages =
        page_size == 1
            ? extend_tokens
            : core::checked_add_u64(core::checked_add_u64(extend_tokens / page_size, batch_size, "HiCache extend allocator gate exceeds uint64 range"),
                                    1,
                                    "HiCache extend allocator gate exceeds uint64 range");
    if (needed_pages > free_pages) merge_release_pages();
}

/** @brief Makes pending releases visible when required by the page request. */
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

/** @brief Releases pages directly or through the queue according to `need_sort`. */
uint64_t HiCacheState::DeviceAllocatorLedger::release(uint64_t pages) {
    if (pages == 0 || capacity_pages == 0) return 0;
    const auto room = capacity_pages > available_pages() ? capacity_pages - available_pages() : 0;
    const auto released = std::min(pages, room);
    if (need_sort) release_pages = core::checked_add_u64(release_pages, released, "HiCache release queue exceeds uint64 range");
    else free_pages = core::checked_add_u64(free_pages, released, "HiCache allocator free page count exceeds uint64 range");
    return released;
}

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)), pager_(config_), policy_(config_) {}

std::string HiCacheState::normalized_scope(const HiCacheFact & fact) const { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

std::string HiCacheState::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

void HiCacheState::register_prefetch_control_boundary(const HiCacheFact & fact) {
    auto register_request = [&](const std::string & request_id) {
        if (request_id.empty()) return;
        HiCacheFact boundary;
        boundary.source_node_id = fact.source_node_id;
        boundary.execution_anchor_node_id = fact.execution_anchor_node_id;
        boundary.source_event_index = fact.source_event_index;
        boundary.ts = fact.ts;
        boundary.event_name = fact.event_name;
        boundary.role = fact.role;
        boundary.phase = fact.phase;
        boundary.request_id = request_id;
        boundary.cache_scope = fact.cache_scope;
        prefetch_control_boundaries_[scoped_request_key(boundary)].push_back(std::move(boundary));
    };
    if (fact.batch_paths.empty()) {
        register_request(fact.request_id);
        return;
    }
    for (const auto & entry : fact.batch_paths) { register_request(entry.request_id); }
}

const HiCacheFact * HiCacheState::prefetch_control_boundary_for_lookup(const HiCacheFact & fact) const {
    const auto request_key = scoped_request_key(fact);
    const auto boundaries = prefetch_control_boundaries_.find(request_key);
    if (boundaries == prefetch_control_boundaries_.end()) return nullptr;
    const HiCacheFact * selected = nullptr;
    for (const auto & candidate : boundaries->second) {
        if (candidate.ts < fact.ts) continue;
        if (selected == nullptr || candidate.ts < selected->ts || (candidate.ts == selected->ts && candidate.source_event_index < selected->source_event_index))
            selected = &candidate;
    }
    return selected;
}

const HiCacheFact * HiCacheState::prefetch_control_boundary_for_operation(const HiCachePrefetchOperation & operation) const {
    const auto boundaries = prefetch_control_boundaries_.find(operation.header.request_key);
    if (boundaries == prefetch_control_boundaries_.end()) return nullptr;
    const HiCacheFact * selected = nullptr;
    for (const auto & candidate : boundaries->second) {
        if (candidate.ts < operation.header.enqueue_ts) continue;
        if (selected == nullptr || candidate.ts < selected->ts || (candidate.ts == selected->ts && candidate.source_event_index < selected->source_event_index))
            selected = &candidate;
    }
    return selected;
}

HiCacheState::ScopedState & HiCacheState::scope_state(const HiCacheFact & fact) { return scopes_[normalized_scope(fact)]; }

void HiCacheState::ensure_device_allocator(ScopedState & scope) {
    scope.device_allocator.configure(policy_.l1_capacity_pages(), policy_.device_allocator_need_sort());
}

/**
 * @brief Reports whether newly inserted device pages remain observably dirty.
 *
 * Write-back retains dirty state. Selective write-through with threshold one backs
 * up at the same insertion boundary, so final and transition views must not expose
 * a transient dirty state.
 */
bool HiCacheState::inserted_device_dirty_visible_at_insert_boundary() const {
    if (policy_.write_back_enabled()) return true;
    return policy_.write_count_enabled() && policy_.write_through_threshold() > 1;
}

/**
 * @brief Records token-path resolution state in the Debug summary.
 *
 * Missing paths, phase errors, and diagnostic counters are centralized here so
 * individual role handlers do not reinterpret token-directory failure semantics.
 */
void HiCacheState::record_token_resolution(const HiCacheFact & fact, HiCacheSummary & summary, const HiCacheTokenResolution & resolution) const {
#ifdef DEBUG
    const auto status = hicache_token_resolution_status_name(resolution.status);
    (void)core::checked_increment_u64(summary.token_resolution_by_status[status], "HiCache token-resolution status count exceeds uint64 range");
    (void)core::checked_increment_u64(summary.token_path_diagnostics[fact.role + "." + status], "HiCache token-path diagnostic count exceeds uint64 range");

    if (!resolution.ok()) {
        (void)core::checked_increment_u64(summary.missing_state_model_facts["token_resolution_" + status],
                                          "HiCache missing state-model fact count exceeds uint64 range");
        if (fact.role == "cache_lifecycle_commit" && resolution.status == HiCacheTokenResolutionStatus::Missing)
            (void)core::checked_increment_u64(summary.token_path_diagnostics["cache_lifecycle_commit_missing_committed_path_count"],
                                              "HiCache token-path diagnostic count exceeds uint64 range");
        return;
    }

    if (fact.role == "prefetch_candidate_anchor")
        (void)core::checked_increment_u64(summary.token_path_diagnostics["prefetch_candidate_path_not_committed_count"],
                                          "HiCache token-path diagnostic count exceeds uint64 range");
    if (fact.role == "cache_lifecycle_commit") {
        const auto * previous = token_directory_.previous_committed_snapshot(fact);
        if (previous != nullptr && resolution.page_aligned_token_count > previous->page_aligned_token_count)
            (void)core::checked_increment_u64(summary.token_path_diagnostics["lifecycle_path_growth_cross_page_boundary_count"],
                                              "HiCache token-path diagnostic count exceeds uint64 range");
    }
#else
    (void)fact;
    (void)summary;
    (void)resolution;
#endif
}

HiCachePagePath HiCacheState::page_path_from_resolution(const HiCacheFact & fact, const HiCacheTokenResolution & resolution) const {
    if (!resolution.ok() || resolution.tokens.empty()) return {};
    return pager_.project(fact, resolution.tokens);
}

#ifdef DEBUG
std::string HiCacheState::digest() const { return derived_state().digest(); }

/** @brief Derives final validation state from every scope's canonical runtime state. */
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
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.refs.active_owner_count(), "HiCache active reference owner count exceeds uint64 range");
    return count;
}

uint64_t HiCacheState::radix_split_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.tree.split_count(), "HiCache radix split count exceeds uint64 range");
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

uint64_t HiCacheState::control_boundary_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.clock.boundary_count(), "HiCache control boundary count exceeds uint64 range");
    return count;
}

std::vector<HiCacheControlBoundary> HiCacheState::control_boundary_trace() const {
    std::vector<HiCacheControlBoundary> boundaries;
    for (const auto & scope : scopes_ | std::views::values) {
        const auto & scope_boundaries = scope.clock.boundaries();
        boundaries.insert(boundaries.end(), scope_boundaries.begin(), scope_boundaries.end());
    }
    std::ranges::sort(boundaries, [](const auto & left, const auto & right) {
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.scheduler_epoch != right.scheduler_epoch) return left.scheduler_epoch < right.scheduler_epoch;
        return left.boundary_epoch < right.boundary_epoch;
    });
    return boundaries;
}

uint64_t HiCacheState::async_lifecycle_transition_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count =
            core::checked_add_u64(count, scope.async_ops.lifecycle_transition_count(), "HiCache asynchronous lifecycle transition count exceeds uint64 range");
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

uint64_t HiCacheState::policy_decision_count() const { return policy_decision_epoch_; }

uint64_t HiCacheState::storage_known_page_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.storage.known_page_count(), "HiCache known storage page count exceeds uint64 range");
    return count;
}

uint64_t HiCacheState::storage_readable_page_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.storage.readable_page_count(), "HiCache readable storage page count exceeds uint64 range");
    return count;
}

uint64_t HiCacheState::storage_backend_readable_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.storage.backend_readable_count(), "HiCache readable storage backend count exceeds uint64 range");
    return count;
}

uint64_t HiCacheState::storage_materialized_page_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.storage.materialized_page_count(), "HiCache materialized storage page count exceeds uint64 range");
    return count;
}

uint64_t HiCacheState::capacity_mutation_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.capacity.mutation_epoch(), "HiCache capacity mutation count exceeds uint64 range");
    return count;
}

uint64_t HiCacheState::capacity_victim_choice_count() const {
    uint64_t count = 0;
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.capacity.victim_selection_count(), "HiCache victim selection count exceeds uint64 range");
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

/** @brief Collects capacity-index consistency issues across all scopes. */
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
    for (const auto & scope : scopes_ | std::views::values)
        count = core::checked_add_u64(count, scope.refs.mutation_count(), "HiCache reference mutation count exceeds uint64 range");
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

/** @brief Collects reference-ledger consistency issues across all scopes. */
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
#endif

/**
 * @brief Synchronizes canonical tree, references, and reservations into capacity.
 *
 * The capacity index is a derived eviction and summary index. Explicit synchronization
 * after every relevant mutation avoids full-tree rescans and prevents it from becoming
 * a hidden source of state.
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

/** @brief Synchronizes terminal, ancestor, new, and restored nodes after insertion. */
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

/** @brief Synchronizes capacity eligibility for nodes affected by a reference change. */
void HiCacheState::sync_capacity_for_ref(ScopedState & scope, const std::string & cache_scope, const HiCacheRefChange & change, const std::string & reason) {
    std::set<HiCacheNodeId> nodes{ change.affected_nodes.begin(), change.affected_nodes.end() };
    sync_capacity(scope, cache_scope, { nodes.begin(), nodes.end() }, reason);
}

/** @brief Assigns a global epoch and source identity to one policy decision. */
void HiCacheState::record_policy_decision(const HiCacheFact & fact, HiCachePolicyDecisionRecord && decision) {
#ifdef DEBUG
    decision.decision_epoch = core::checked_increment_u64(policy_decision_epoch_, "HiCache policy decision epoch exceeds uint64 range");
    decision.cache_scope = normalized_scope(fact);
    decision.request_key = scoped_request_key(fact);
    decision.role = fact.role;
    decision.event_name = fact.event_name;
    policy_decisions_.push_back(std::move(decision));
#else
    (void)fact;
    (void)decision;
#endif
}

/**
 * @brief Records one state transition and optional before/after digests.
 *
 * Empty page transitions are omitted unless the transition itself is a lifecycle
 * event such as reference release or suppressed/late prefetch. This keeps the trace
 * auditable without filling it with no-op page sets.
 */
void HiCacheState::record_transition(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                     const TransitionDescriptor & descriptor, const std::vector<std::string> & pages, const std::string & before_digest) {
#ifdef DEBUG
    if (pages.empty() && descriptor.kind != "release_ref" && descriptor.kind != "prefetch_suppressed" && descriptor.kind != "prefetch_late") return;
    HiCacheStateTransition transition;
    const auto transition_offset =
        core::checked_add_u64(summary.state_transition_count, static_cast<uint64_t>(transitions.size()), "HiCache transition sequence exceeds uint64 range");
    transition.transition_id = std::to_string(core::checked_add_u64(transition_offset, 1, "HiCache transition sequence exceeds uint64 range"));
    transition.kind = descriptor.kind;
    transition.role = fact.role;
    transition.request_id = fact.request_id;
    transition.operation_id = fact.operation_id;
    transition.event_name = fact.event_name;
    transition.cache_scope = normalized_scope(fact);
    transition.ts = fact.ts;
    transition.source_event_index = fact.source_event_index;
    transition.tier = descriptor.tier;
    transition.pages = pages;
    if (config_.emit_state_digests) {
        transition.before_state_digest = before_digest;
        transition.after_state_digest = digest();
    }
    (void)core::checked_increment_u64(summary.transitions_by_kind[std::string(descriptor.kind)], "HiCache transition-kind count exceeds uint64 range");
    transitions.push_back(std::move(transition));
#else
    (void)fact;
    (void)summary;
    (void)transitions;
    (void)descriptor;
    (void)pages;
    (void)before_digest;
#endif
}

/**
 * @brief Dispatches one consumable fact through the canonical state machine.
 *
 * Facts first update the token directory, then drain write-through acknowledgements
 * at the target-control boundary, and only then enter their role handler. Every
 * handler therefore observes the same token timeline and reference/capacity baseline.
 */
void HiCacheState::apply_fact(const HiCacheFact & fact, HiCacheFactRole role, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions) {
#ifdef DEBUG
    token_directory_.observe_fact_path(fact, pager_.page_size_for_fact(fact));
#endif
    if (role != HiCacheFactRole::Unknown) {
        auto & scope = scope_state(fact);
        drain_write_through_backup_refs(fact, summary, transitions, scope, "write_through_backup_ack_boundary");
        advance_ready_prefetches(fact, summary, transitions);
    }
    observe_effect_opportunities(fact, role);

    switch (role) {
    case HiCacheFactRole::PrefetchCandidateAnchor:
        apply_prefetch_candidate_anchor(fact, summary, transitions);
        break;
    case HiCacheFactRole::CacheLookupInput:
        apply_cache_lookup_input(fact, summary, transitions);
        break;
    case HiCacheFactRole::CacheExtendInput:
        apply_cache_extend_input(fact, summary, transitions);
        break;
    case HiCacheFactRole::CacheLifecycleCommit:
        apply_cache_lifecycle_commit(fact, summary, transitions);
        break;
    case HiCacheFactRole::Unknown:
        break;
    }
}


} // namespace markov::trace_graph::modules::hicache::model
