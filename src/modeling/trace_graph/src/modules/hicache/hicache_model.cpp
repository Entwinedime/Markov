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

bool has_backup(const HiCacheCacheNode & node) { return node.residency.host_present || node.residency.storage_readable; }

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

HiCacheTokenCompleteness completeness_for_fact(const HiCacheFact & fact, uint64_t page_size) {
    if (fact.full_path_tokens.empty()) return HiCacheTokenCompleteness::Unknown;
    if (fact.full_path_span.valid && fact.full_path_span.token_count == fact.full_path_tokens.size()) return HiCacheTokenCompleteness::Full;
    if (page_size > 0 && fact.full_path_tokens.size() / page_size * page_size == fact.full_path_tokens.size()) return HiCacheTokenCompleteness::PageAligned;
    return HiCacheTokenCompleteness::Partial;
}

} // namespace

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)), pager_(config_), policy_(config_) {}

std::string HiCacheState::normalized_scope(const HiCacheFact & fact) const { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

std::string HiCacheState::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

HiCacheState::ScopedState & HiCacheState::scope_state(const HiCacheFact & fact) { return scopes_[normalized_scope(fact)]; }

HiCacheTokenPath HiCacheState::tokens_for_fact(const HiCacheFact & fact, HiCacheSummary & summary) const {
    if (!fact.full_path_tokens.empty()) return fact.full_path_tokens;
    const auto stored = token_store_.request_tokens(fact);
    if (!stored.empty()) return stored;
    if (fact.token_count >= pager_.page_size_for_fact(fact)) summary.missing_invariant_facts["token_dictionary_or_full_path_span"]++;
    return {};
}

HiCachePagePath HiCacheState::page_path_for_fact(const HiCacheFact & fact, HiCacheSummary & summary) const {
    const auto tokens = tokens_for_fact(fact, summary);
    if (tokens.empty()) return {};
    return pager_.project(fact, tokens);
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
    if (!fact.full_path_tokens.empty()) {
        token_store_.set_request_tokens(fact, fact.full_path_tokens, completeness_for_fact(fact, pager_.page_size_for_fact(fact)));
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
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "request_lookup_touch");
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
    const auto tokens = tokens_for_fact(fact, summary);
    if (!tokens.empty()) token_store_.observe_request_bound_tokens(fact, tokens);
    const auto page_path = pager_.project(fact, tokens);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;

    auto & scope = scope_state(fact);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
    auto lookup = scope.tree.lookup(pages);
    scope.tree.touch_chain(lookup.device_chain);
    scope.tree.touch_chain(lookup.host_chain);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "match_anchor_touch");

    /**
     * @brief modeled load-back：已经在 host/storage 可见但 device 缺失的连续前缀，
     * 在 request lookup 边界可以重新 materialize 到 L1。
     */
    if (lookup.visible_pages.size() > lookup.device_pages.size()) {
        const auto loadback_id = scope.clock.next_operation_id("loadback");
        const auto loadback_owner = scoped_request_key(fact) + ":loadback:" + loadback_id;
        const auto before_enqueue = digest();
        scope.async_ops.upsert_loadback(HiCacheLoadbackOperation{
            .header = make_operation_header(HiCacheOperationKind::Loadback,
                                            loadback_id,
                                            normalized_scope(fact),
                                            scoped_request_key(fact),
                                            loadback_owner,
                                            lookup.terminal_node,
                                            lookup.topology_chain,
                                            lookup.visible_pages,
                                            fact.ts,
                                            0),
            .target_node = lookup.terminal_node,
        });
        auto ref = scope.refs.acquire_lock(scope.tree, loadback_owner, "loadback", scoped_request_key(fact), loadback_id, lookup.topology_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "loadback_ref_acquire");
        record_transition(fact, summary, transitions, "enqueue_loadback", "loadback", lookup.visible_pages, before_enqueue);
        const auto before = digest();
        auto promoted = prefix_to(lookup.visible_pages, lookup.visible_pages.size());
        auto insert = scope.tree.insert_device_path(promoted, fact.priority, false);
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
        record_transition(fact, summary, transitions, "complete_loadback", "loadback", lookup.visible_pages, before_complete);
    }

    update_request_state(fact, scope, pages);
    enforce_device_capacity(fact, summary, transitions, scope, 0);
    enforce_host_capacity(fact, summary, transitions, scope, 0);
}

void HiCacheState::apply_request_admission(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto page_path = page_path_for_fact(fact, summary);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
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
    request.device_reservation_pages =
        request.full_pages.size() > request.device_pages.size() ? static_cast<uint64_t>(request.full_pages.size() - request.device_pages.size()) : 0;
    request.lifecycle_state = "admitted";
    record_transition(fact, summary, transitions, "acquire_request_ref", "node_ref", request.device_pages, digest());
    enforce_device_capacity(fact, summary, transitions, scope, request.device_reservation_pages);
}

void HiCacheState::insert_request_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                       ScopedState & scope, const std::vector<std::string> & pages) {
    const auto before = digest();
    const auto insert = scope.tree.insert_device_path(pages, fact.priority, policy_.write_back_enabled());
    sync_capacity_for_insert(scope, normalized_scope(fact), insert, "request_insert_device");
    auto new_pages = flatten_node_pages(scope.tree, insert.new_device_nodes);
    auto restored_pages = flatten_node_pages(scope.tree, insert.restored_device_nodes);
    record_transition(fact, summary, transitions, "add_l1_residency", "L1", new_pages, before);
    record_transition(fact, summary, transitions, "restore_l1_residency", "L1", restored_pages, before);
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "write_policy",
                               .policy_name = policy_.write_policy(),
                               .decision = policy_.write_back_enabled() ? "mark_new_device_pages_dirty" : "defer_to_hit_count_backup",
                               .reason = policy_.write_back_enabled() ? "write_back stores fresh pages as dirty" : "write-through policy uses hit-count backup",
                               .accepted = policy_.write_back_enabled(),
                               .candidate_pages = static_cast<uint64_t>(new_pages.size()),
                               .threshold_pages = policy_.write_through_threshold(),
                               .pages = new_pages,
                           });
    if (policy_.write_back_enabled()) record_transition(fact, summary, transitions, "mark_dirty", "dirty", new_pages, before);
}

void HiCacheState::apply_request_lifecycle_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto kind = lower_copy(fact.lifecycle_kind);
    if (!kind.empty() && kind != "finished" && kind != "unfinished") return;

    const auto page_path = page_path_for_fact(fact, summary);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
    insert_request_path(fact, summary, transitions, scope, pages);
    apply_write_count_policy(fact, summary, transitions, scope, pages);
    update_request_state(fact, scope, pages);

    const auto key = scoped_request_key(fact);
    auto it = scope.requests.find(key);
    if (it == scope.requests.end()) return;
    it->second.device_reservation_pages = 0;
    if (kind == "unfinished") {
        it->second.lifecycle_state = "unfinished";
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
    enforce_device_capacity(fact, summary, transitions, scope, 0);
    enforce_host_capacity(fact, summary, transitions, scope, 0);
}

void HiCacheState::commit_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      ScopedState & scope, HiCacheNodeId node_id, bool storage_readable) {
    const auto pages = scope.tree.node_pages(node_id);
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
    if (!storage_id.empty()) {
        const auto before_complete = digest();
        scope.async_ops.set_storage_state(storage_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
        const auto ref = scope.refs.release_owner(scope.tree, scoped_request_key(fact) + ":storage:" + storage_id);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "storage_ref_release");
        record_transition(fact, summary, transitions, "complete_storage_backup", "storage", pages, before_complete);
    }
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

    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "write_count_lookup_touch");
    for (const auto node_id : lookup.topology_chain) {
        auto * node = scope.tree.mutable_node(node_id);
        if (node == nullptr || !node->residency.device_present) continue;
        const auto before = digest();
        node->hit_count++;
        record_transition(fact, summary, transitions, "increment_hit_count", "hit_count", node->pages, before);
        const auto should_backup = !has_backup(*node) && node->hit_count >= threshold;
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "write_policy",
                                   .policy_name = policy_.write_policy(),
                                   .decision = should_backup ? "commit_hit_count_backup" : "wait_for_hit_count_backup",
                                   .reason = has_backup(*node) ? "node already has host/storage backup"
                                             : should_backup   ? "node hit count reached write-through threshold"
                                                               : "node hit count is below write-through threshold",
                                   .accepted = should_backup,
                                   .candidate_pages = static_cast<uint64_t>(node->pages.size()),
                                   .hit_count = node->hit_count,
                                   .threshold_pages = threshold,
                                   .pages = node->pages,
                               });
        if (should_backup) commit_host_backup(fact, summary, transitions, scope, node_id, true);
    }
}

void HiCacheState::evict_device_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                                     HiCacheNodeId node_id) {
    auto * node = scope.tree.mutable_node(node_id);
    if (node == nullptr || !node->residency.device_present) return;

    const auto pages = node->pages;
    const bool needs_writeback = policy_.write_back_enabled() && !has_backup(*node);
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .policy_area = "write_policy",
                               .policy_name = policy_.write_policy(),
                               .decision = needs_writeback ? "enqueue_dirty_eviction_writeback" : "evict_without_writeback",
                               .reason = needs_writeback     ? "write_back dirty node has no host/storage backup"
                                         : has_backup(*node) ? "node already has host/storage backup"
                                                             : "target write policy does not require dirty eviction writeback",
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
        commit_host_backup(fact, summary, transitions, scope, node_id, true);
        const auto before_complete = digest();
        scope.async_ops.set_writeback_state(writeback_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
        ref = scope.refs.release_owner(scope.tree, writeback_owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "writeback_ref_release");
        record_transition(fact, summary, transitions, "complete_writeback", "writeback", pages, before_complete);
    }

    const auto before = digest();
    if (has_backup(*node)) scope.tree.demote_device_to_host(node_id, false);
    else scope.tree.remove_device_regular(node_id);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "evict_device_node");
    record_transition(fact, summary, transitions, "evict_l1_node", "L1", pages, before);
}

void HiCacheState::evict_host_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                                   HiCacheNodeId node_id) {
    const auto pages = scope.tree.node_pages(node_id);
    const auto before = digest();
    scope.tree.remove_host(node_id);
    sync_capacity(scope, normalized_scope(fact), std::vector<HiCacheNodeId>{ node_id }, "evict_host_node");
    record_transition(fact, summary, transitions, "evict_host_node", "L2", pages, before);
}

void HiCacheState::enforce_device_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                           ScopedState & scope, uint64_t requested_pages) {
    const auto capacity = policy_.l1_capacity_pages();
    if (capacity == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, "device_capacity_budget");
    auto target = std::max(requested_pages, scope.capacity.device_excess_pages(capacity));
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {}, "device_capacity_loop");
        const auto victim = scope.capacity.select_device_victim(capacity, requested_pages, "device_capacity_loop");
        if (!victim) break;
        const auto victim_pages = scope.tree.node_pages(*victim).size();
        evict_device_node(fact, summary, transitions, scope, *victim);
        if (victim_pages >= target) break;
        target -= victim_pages;
    }
}

void HiCacheState::enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         ScopedState & scope, uint64_t requested_pages) {
    const auto capacity = policy_.l2_capacity_pages();
    if (capacity == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, "host_capacity_budget");
    auto target = std::max(requested_pages, scope.capacity.host_excess_pages(capacity));
    while (target > 0) {
        sync_capacity(scope, normalized_scope(fact), {}, "host_capacity_loop");
        const auto victim = scope.capacity.select_host_victim(capacity, requested_pages, "host_capacity_loop");
        if (!victim) break;
        const auto victim_pages = scope.tree.node_pages(*victim).size();
        evict_host_node(fact, summary, transitions, scope, *victim);
        if (victim_pages >= target) break;
        target -= victim_pages;
    }
}

void HiCacheState::apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto page_path = page_path_for_fact(fact, summary);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    scope.storage.observe_path(page_path);
    scope.tree.observe_page_path(page_path);
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

    enforce_host_capacity(fact, summary, transitions, scope, requested_pages);
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_capacity_budget");
    const auto capacity_snapshot = scope.capacity.snapshot();
    const auto capacity = policy_.l2_capacity_pages();
    const auto committed_host_pages = capacity_snapshot.occupied_host_pages + capacity_snapshot.reserved_host_pages;
    if (capacity > 0 && committed_host_pages >= capacity) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "prefetch_enqueue",
                                   .policy_name = policy_.prefetch_policy(),
                                   .decision = "skip_prefetch",
                                   .reason = "target host capacity has no reservable space after cleanup",
                                   .accepted = false,
                                   .requested_pages = requested_pages,
                                   .candidate_pages = requested_pages,
                                   .active_requested_pages = active_requested_pages,
                                   .capacity_pages = capacity,
                                   .occupied_pages = capacity_snapshot.occupied_host_pages,
                                   .reserved_pages = capacity_snapshot.reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .limit_pages = policy_.prefetch_capacity_limit_pages(),
                                   .pages = planned_pages,
                               });
        return;
    }
    auto reservable = capacity == 0 ? requested_pages : std::min<uint64_t>(requested_pages, capacity - committed_host_pages);
    if (reservable < policy_.prefetch_threshold_pages()) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "prefetch_enqueue",
                                   .policy_name = policy_.prefetch_policy(),
                                   .decision = "skip_prefetch",
                                   .reason = "reservable host pages are below prefetch threshold",
                                   .accepted = false,
                                   .requested_pages = requested_pages,
                                   .candidate_pages = reservable,
                                   .active_requested_pages = active_requested_pages,
                                   .capacity_pages = capacity,
                                   .occupied_pages = capacity_snapshot.occupied_host_pages,
                                   .reserved_pages = capacity_snapshot.reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .limit_pages = policy_.prefetch_capacity_limit_pages(),
                                   .pages = planned_pages,
                               });
        return;
    }
    if (planned_pages.size() > reservable) {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .policy_area = "prefetch_enqueue",
                                   .policy_name = policy_.prefetch_policy(),
                                   .decision = "truncate_prefetch_reservation",
                                   .reason = "host capacity allows only a prefix of planned prefetch pages",
                                   .accepted = true,
                                   .requested_pages = requested_pages,
                                   .candidate_pages = reservable,
                                   .active_requested_pages = active_requested_pages,
                                   .capacity_pages = capacity,
                                   .occupied_pages = capacity_snapshot.occupied_host_pages,
                                   .reserved_pages = capacity_snapshot.reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .limit_pages = policy_.prefetch_capacity_limit_pages(),
                                   .pages = planned_pages,
                               });
        planned_pages.resize(static_cast<size_t>(reservable));
        planned_projected_pages.resize(static_cast<size_t>(reservable));
    }

    const auto hit_pages = storage_hit_prefix(scope.storage, planned_projected_pages);
    const auto request_key = scoped_request_key(fact);
    if (request_key.empty()) return;
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

    const auto prefetch_id = scope.clock.next_operation_id("prefetch");
    const auto enqueue_epoch = scope.clock.next_enqueue_epoch();
    auto owner = request_key + ":" + prefetch_id;
    if (!lookup.host_chain.empty()) {
        const auto ref = scope.refs.acquire_host(scope.tree, owner, "prefetch", request_key, prefetch_id, lookup.host_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_anchor_ref_acquire");
    }
    HiCachePrefetchOperation op{
        .header = make_operation_header(HiCacheOperationKind::Prefetch,
                                        prefetch_id,
                                        normalized_scope(fact),
                                        request_key,
                                        owner,
                                        lookup.deepest_host_node,
                                        lookup.host_chain,
                                        planned_pages,
                                        fact.ts,
                                        enqueue_epoch),
        .anchor_chain = lookup.host_chain,
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
                               .occupied_pages = capacity_snapshot.occupied_host_pages,
                               .reserved_pages = reservable,
                               .threshold_pages = policy_.prefetch_threshold_pages(),
                               .limit_pages = policy_.prefetch_capacity_limit_pages(),
                               .pages = planned_pages,
                           });
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_reservation");
    record_transition(fact, summary, transitions, "prefetch_planned", "prefetch", planned_pages, digest());
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

    auto release_op_refs = [&] {
        const auto ref = scope.refs.release_owner(scope.tree, op->header.owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "prefetch_ref_release");
    };
    auto suppress = [&](const std::string & kind, HiCachePrefetchState state) {
        const auto before = digest();
        scope.async_ops.set_prefetch_state_by_id(op->header.operation_id, state, HiCacheOperationState::Cancelled, kind, fact.ts);
        op->reserved_host_pages = 0;
        release_op_refs();
        sync_capacity(scope, normalized_scope(fact), {}, "prefetch_cancel_reservation");
        record_transition(fact, summary, transitions, kind, "prefetch", op->planned_pages, before);
    };
    auto apply_ready = [&] {
        const auto before_ready = digest();
        scope.async_ops.set_prefetch_state_by_id(op->header.operation_id,
                                                 HiCachePrefetchState::Ready,
                                                 HiCacheOperationState::Ready,
                                                 "storage_hit_ready",
                                                 fact.ts);
        record_transition(fact, summary, transitions, "prefetch_ready", "prefetch", op->hit_pages, before_ready);
        const auto before_apply = digest();
        const auto visible_pages = std::set<std::string>(op->hit_pages.begin(), op->hit_pages.end());
        (void)scope.tree.lookup(prefix_to(op->host_insert_pages, static_cast<size_t>(op->host_visible_offset_pages)));
        auto insert = scope.tree.insert_host_path(op->host_insert_pages, visible_pages, true);
        sync_capacity_for_insert(scope, normalized_scope(fact), insert, "prefetch_insert_host");
        scope.storage.mark_readable_pages(normalized_scope(fact), op->hit_pages);
        for (const auto node_id : insert.touched_nodes) { scope.storage.mark_materialized_pages(scope.tree.node_pages(node_id), node_id); }
        scope.async_ops.set_prefetch_state_by_id(op->header.operation_id,
                                                 HiCachePrefetchState::Applied,
                                                 HiCacheOperationState::Committed,
                                                 "apply_host_visibility",
                                                 fact.ts);
        op->reserved_host_pages = 0;
        release_op_refs();
        sync_capacity(scope, normalized_scope(fact), {}, "prefetch_apply_reservation");
        record_transition(fact,
                          summary,
                          transitions,
                          "apply_prefetch_host_visibility",
                          "L2",
                          flatten_node_pages(scope.tree, insert.new_host_nodes),
                          before_apply);
        enforce_host_capacity(fact, summary, transitions, scope, 0);
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
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_checkpoint",
                                   .policy_name = policy,
                                   .decision = terminal ? "apply_prefetch" : "wait_for_completion",
                                   .reason = terminal ? "wait_complete checkpoint is terminal" : "wait_complete requires terminal checkpoint",
                                   .accepted = terminal,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
        if (terminal) apply_ready();
        return;
    }
    if (policy == "best_effort") {
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_checkpoint",
                                   .policy_name = policy,
                                   .decision = "apply_prefetch",
                                   .reason = "best_effort applies once storage hit prefix passes threshold",
                                   .accepted = true,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
        apply_ready();
        return;
    }
    if (policy == "timeout") {
        const auto terminal = policy_.terminal_prefetch_checkpoint(fact.check_kind);
        const auto timeout_elapsed =
            policy_.prefetch_timeout_elapsed(op->header.enqueue_ts, fact.ts, static_cast<uint64_t>(op->planned_pages.size() * config_.page_size));
        record_policy_decision(fact,
                               HiCachePolicyDecisionRecord{
                                   .operation_id = op->header.operation_id,
                                   .policy_area = "prefetch_checkpoint",
                                   .policy_name = policy,
                                   .decision = terminal || timeout_elapsed ? "apply_prefetch" : "wait_for_timeout_or_completion",
                                   .reason = terminal          ? "timeout policy checkpoint is terminal"
                                             : timeout_elapsed ? "target timeout elapsed"
                                                               : "timeout policy waits for terminal checkpoint or elapsed timeout",
                                   .accepted = terminal || timeout_elapsed,
                                   .requested_pages = op->requested_host_pages,
                                   .candidate_pages = static_cast<uint64_t>(op->planned_pages.size()),
                                   .hit_pages = static_cast<uint64_t>(op->hit_pages.size()),
                                   .reserved_pages = op->reserved_host_pages,
                                   .threshold_pages = policy_.prefetch_threshold_pages(),
                                   .pages = op->planned_pages,
                               });
        if (terminal || timeout_elapsed) { apply_ready(); }
        return;
    }
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
                               .reserved_pages = op->reserved_host_pages,
                               .threshold_pages = policy_.prefetch_threshold_pages(),
                               .pages = op->planned_pages,
                           });
    apply_ready();
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
