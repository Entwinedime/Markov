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

/**
 * @brief Restores the allocator/tree ownership invariant at a lifecycle commit.
 *
 * Device pages are owned either by the committed radix tree or by requests whose
 * allocation has not yet become radix-visible.  Reconciliation is derived from
 * those two ownership ledgers; it does not depend on a workload or configuration
 * name.  An existing delayed-release queue is preserved when the target allocator
 * requires sorting before reuse.
 */
void HiCacheState::DeviceAllocatorLedger::reconcile_occupied_pages(uint64_t committed_pages, uint64_t request_owned_pages) {
    if (!initialized || capacity_pages == 0) return;
    const auto owned_pages = core::checked_add_u64(committed_pages, request_owned_pages, "HiCache device ownership exceeds uint64 range");
    const auto bounded_owned_pages = std::min(owned_pages, capacity_pages);
    const auto total_available_pages = capacity_pages - bounded_owned_pages;
    release_pages = std::min(release_pages, total_available_pages);
    free_pages = total_available_pages - release_pages;
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

void HiCacheState::begin_formal_window() {
    formal_window_active_ = true;
    formal_boundary_seen_ = true;
    prefetch_control_boundaries_.clear();
    for (auto & scope : scopes_ | std::views::values) {
        scope.requests.clear();
        scope.pending_write_through_backups.clear();
        scope.async_ops.clear_operations_for_window_boundary();
    }
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

HiCachePagePath HiCacheState::page_path_from_resolution(const HiCacheFact & fact, const HiCacheTokenResolution & resolution) const {
    if (!resolution.ok() || resolution.tokens.empty()) return {};
    return pager_.project(fact, resolution.tokens);
}


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


/**
 * @brief Dispatches one consumable fact through the canonical state machine.
 *
 * Facts first update the token directory, then drain write-through acknowledgements
 * at the target-control boundary, and only then enter their role handler. Every
 * handler therefore observes the same token timeline and reference/capacity baseline.
 */
void HiCacheState::apply_fact(const HiCacheFact & fact, HiCacheFactRole role, bool observe_effects) {
    // A graph without explicit prelude context never calls begin_formal_window().
    // Mark its first modeled fact as formal before a handler can create storage I/O,
    // otherwise ordinary formal writes would be mislabeled as prelude pressure.
    if (observe_effects) formal_window_active_ = true;
    if (role != HiCacheFactRole::Unknown) {
        auto & scope = scope_state(fact);
        drain_write_through_backup_refs(fact, scope, "write_through_backup_ack_boundary");
        advance_ready_prefetches(fact);
    }
    if (observe_effects) observe_effect_opportunities(fact, role);

    switch (role) {
    case HiCacheFactRole::PrefetchCandidateAnchor:
        apply_prefetch_candidate_anchor(fact);
        break;
    case HiCacheFactRole::CacheLookupInput:
        apply_cache_lookup_input(fact);
        break;
    case HiCacheFactRole::CacheExtendInput:
        apply_cache_extend_input(fact);
        break;
    case HiCacheFactRole::CacheLifecycleCommit:
        apply_cache_lifecycle_commit(fact);
        break;
    case HiCacheFactRole::Unknown:
        break;
    }
}



} // namespace markov::trace_graph::modules::hicache::model
