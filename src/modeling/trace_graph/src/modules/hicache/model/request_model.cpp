/**
 * @file
 * @brief Advances HiCache lookup, extend, and lifecycle-commit state.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

namespace {

std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string request_ref_owner(const std::string & request_key) { return request_key.empty() ? std::string{} : request_key + ":request"; }

HiCacheFact batch_request_fact(const HiCacheFact & batch, const std::string & request_id, uint64_t token_count) {
    HiCacheFact fact;
    fact.source_node_id = batch.source_node_id;
    fact.source_event_index = batch.source_event_index;
    fact.ts = batch.ts;
    fact.dur = batch.dur;
    fact.event_name = batch.event_name;
    fact.target_id = batch.target_id;
    fact.fact_class = batch.fact_class;
    fact.role = batch.role;
    fact.phase = batch.phase;
    fact.request_id = request_id;
    fact.operation_id = batch.operation_id;
    fact.cache_scope = batch.cache_scope;
    fact.seq_no = batch.seq_no;
    fact.source_page_size = batch.source_page_size;
    fact.token_count = token_count;
    fact.priority = batch.priority;
    fact.is_start = batch.is_start;
    fact.is_end = batch.is_end;
    return fact;
}

} // namespace

/**
 * @brief Refreshes the request-local view from a post-fact radix lookup.
 *
 * Request state caches only the path and chains visible to that request for later
 * extend and release. Canonical radix residency and references remain authoritative.
 */
void HiCacheState::update_request_state(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages) {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto lookup = scope.tree.lookup_peek(pages);
    auto & request = scope.requests[key];
    request.full_pages = pages;
    request.device_pages = lookup.device_pages;
    request.host_pages = lookup.host_pages;
    request.device_chain = lookup.device_chain;
    request.host_chain = lookup.host_chain;
}

/**
 * @brief Handles request matching and safely loads a host-visible prefix into L1.
 *
 * This boundary precedes cache extend and consumes only host residency already visible
 * in target radix. Storage readability alone cannot invent an H2D loadback. Without
 * scheduler loadback intent, insufficient device capacity skips promotion rather than
 * triggering eviction.
 */
void HiCacheState::apply_cache_lookup_input(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions) {
    const auto resolution = token_directory_.resolve_cache_lookup_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;

    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);
    scope.storage.observe_path(page_path);
#ifdef DEBUG
    scope.tree.observe_page_path(page_path);
#endif
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "cache_lookup_touch");

    const auto request_key = scoped_request_key(fact);
    /**
     * @brief Restricts synchronous loadback to an already host-visible prefix.
     *
     * L3 readability does not prove that this request completed H2D transfer. Host
     * visibility in radix is therefore required before materializing L1 residency.
     */
    const auto promotable_pages = lookup.host_pages;
    if (promotable_pages.size() > lookup.device_pages.size()) {
        const auto loadback_pages = static_cast<uint64_t>(promotable_pages.size() - lookup.device_pages.size());
        scope.device_allocator.merge_before_page_allocation(loadback_pages);
        if (!scope.device_allocator.can_allocate(loadback_pages)) {
            if constexpr (debug_records_enabled())
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
        if constexpr (debug_records_enabled())
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
        const auto transferred_pages = suffix_from(promotable_pages, lookup.device_pages.size());
        const auto before_enqueue = debug_state_digest();
        scope.async_ops.insert_loadback(HiCacheLoadbackOperation{
            .header = make_operation_header(HiCacheOperationKind::Loadback,
                                            loadback_id,
                                            normalized_scope(fact),
                                            request_key,
                                            loadback_owner,
                                            transferred_pages,
                                            fact.ts,
                                            0),
        });
        auto ref = scope.refs.acquire_lock(scope.tree, loadback_owner, "loadback", request_key, loadback_id, lookup.topology_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "loadback_ref_acquire");
        if constexpr (debug_records_enabled())
            record_transition(fact,
                              summary,
                              transitions,
                              TransitionDescriptor{ .kind = "enqueue_loadback", .tier = "loadback" },
                              promotable_pages,
                              before_enqueue);
        const auto before = debug_state_digest();
        auto insert = scope.tree.insert_device_path(promotable_pages, fact.priority, false);
        sync_capacity_for_insert(scope, normalized_scope(fact), insert, "loadback_insert_device");
        if constexpr (debug_records_enabled())
            record_transition(fact,
                              summary,
                              transitions,
                              TransitionDescriptor{ .kind = "promote_visible_prefix_to_l1", .tier = "L1" },
                              flatten_node_pages(scope.tree, insert.restored_device_nodes),
                              before);
        const auto before_complete = debug_state_digest();
        scope.async_ops.set_loadback_state(loadback_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
        ref = scope.refs.release_owner(scope.tree, loadback_owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "loadback_ref_release");
        if constexpr (debug_records_enabled())
            record_transition(fact,
                              summary,
                              transitions,
                              TransitionDescriptor{ .kind = "complete_loadback", .tier = "loadback" },
                              promotable_pages,
                              before_complete);
    }

    update_request_state(fact, scope, pages);
}

/**
 * @brief Resolves every accepted cache-extend path from the current batch fact.
 *
 * Resolution is deliberately fact-local. A missing or inconsistent entry aborts the
 * complete batch so request history cannot fill a partial target input.
 */
std::optional<std::vector<HiCacheFact>> HiCacheState::resolve_cache_extend_entry_facts(const HiCacheFact & fact, HiCacheSummary & summary,
                                                                                       const HiCacheBatchTokenResolution & batch_resolution) const {
    if (!batch_resolution.ok()) {
#ifdef DEBUG
        (void)core::checked_increment_u64(
            summary.missing_state_model_facts["cache_extend_batch_token_resolution_" + hicache_token_resolution_status_name(batch_resolution.status)],
            "HiCache missing cache-extend fact count exceeds uint64 range");
#else
        (void)summary;
#endif
        return std::nullopt;
    }

    std::vector<HiCacheFact> entry_facts;
    entry_facts.reserve(fact.batch_paths.size());
    for (size_t index = 0; index < fact.batch_paths.size(); ++index) {
        const auto & entry = fact.batch_paths[index];
        auto entry_fact = batch_request_fact(fact, entry.request_id, entry.token_count);
        const auto & resolution = batch_resolution.entries[index];
        record_token_resolution(entry_fact, summary, resolution);
        if (!resolution.ok()) return std::nullopt;
        entry_facts.push_back(std::move(entry_fact));
    }
    return entry_facts;
}

/**
 * @brief Applies the three ordered pre-extend prefetch release boundaries.
 *
 * Existing release work drains first, active operations then settle, and a newly
 * visible storage-control revoke drains last. Keeping these passes separate preserves
 * the scheduler ordering represented by the state model.
 */
void HiCacheState::prepare_prefetch_before_cache_extend(const std::vector<HiCacheFact> & entry_facts, HiCacheSummary & summary,
                                                        HiCacheTransitionBuffer & transitions, ScopedState & scope) {
    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (!key.empty())
            drain_prefetch_pending_release(
                entry_fact,
                summary,
                transitions,
                scope,
                key,
                PrefetchReleaseReasons{
                    .capacity = "prefetch_pre_cache_extend_host_release",
                    .policy = "SGLang drains pre-existing storage-control host release queue before scheduling cache extend allocation",
                });
    }
    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (!key.empty()) settle_prefetch_before_cache_extend(entry_fact, summary, transitions, scope, key);
    }
    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (key.empty()) continue;
        auto * prefetch = scope.async_ops.prefetch_for_request(key);
        if (prefetch == nullptr || !prefetch->release_before_cache_extend) continue;
        prefetch->release_before_cache_extend = false;
        drain_prefetch_pending_release(entry_fact,
                                       summary,
                                       transitions,
                                       scope,
                                       key,
                                       PrefetchReleaseReasons{
                                           .capacity = "prefetch_storage_control_pre_cache_extend_host_release",
                                           .policy = "Target-derived storage-control revoke releases host reservation before cache extend side effects",
                                       });
    }
}

/** @brief Drains releases created by the current cache-extend settlement round. */
void HiCacheState::drain_prefetch_after_cache_extend(const std::vector<HiCacheFact> & entry_facts, HiCacheSummary & summary,
                                                     HiCacheTransitionBuffer & transitions, ScopedState & scope) {
    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (!key.empty())
            drain_prefetch_pending_release(
                entry_fact,
                summary,
                transitions,
                scope,
                key,
                PrefetchReleaseReasons{
                    .capacity = "prefetch_post_cache_extend_host_release",
                    .policy = "Current-round prefetch termination release is drained after cache extend side effects in the target model",
                });
    }
}

/**
 * @brief Rebinds request references and projects batch device-allocation pressure.
 *
 * The start of `ScheduleBatch.prepare_for_extend` has formed accepted fill paths but
 * has not run allocation. This handler computes pressure for the complete batch;
 * lifecycle commit performs radix insertion so uncommitted paths never become canonical.
 */
void HiCacheState::apply_cache_extend_input(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions) {
    const auto page_size = pager_.page_size_for_fact(fact);
    const auto batch_resolution = token_directory_.resolve_cache_extend_paths(fact, page_size);
    const auto entry_facts = resolve_cache_extend_entry_facts(fact, summary, batch_resolution);
    if (!entry_facts) return;

    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);
    prepare_prefetch_before_cache_extend(*entry_facts, summary, transitions, scope);

    CacheExtendBatchIntent batch_intent;
    batch_intent.batch_size = static_cast<uint64_t>(entry_facts->size());
    for (size_t index = 0; index < entry_facts->size(); ++index) {
        const auto & entry_fact = (*entry_facts)[index];
        const auto & resolution = batch_resolution.entries[index];
        auto page_path = page_path_from_resolution(entry_fact, resolution);
        auto pages = page_path.page_ids();
        if (pages.empty()) continue;

        scope.storage.observe_path(page_path);
#ifdef DEBUG
        scope.tree.observe_page_path(page_path);
#endif
        update_request_state(entry_fact, scope, pages);

        const auto key = scoped_request_key(entry_fact);
        if (key.empty()) { continue; }
        auto & request = scope.requests[key];
        const auto device_prefix_pages = static_cast<uint64_t>(request.device_pages.size());
        const auto device_prefix_tokens =
            core::checked_multiply_u64(device_prefix_pages, page_path.page_size, "HiCache device prefix token count exceeds uint64 range");
        const auto prior_committed_prefix_tokens =
            request.lifecycle_state == "unfinished" ? std::min(request.committed_tokens, resolution.token_count) : uint64_t{ 0 };
        const auto allocation_prefix_tokens = std::max(device_prefix_tokens, prior_committed_prefix_tokens);
        auto allocation_intent = make_extend_allocation_intent(detail::ExtendAllocationInput{
            .token_count = resolution.token_count,
            .prefix_tokens = allocation_prefix_tokens,
            .page_size = page_path.page_size,
            .batch_size = 1,
        });
        batch_intent.total_extend_tokens =
            core::checked_add_u64(batch_intent.total_extend_tokens, allocation_intent.extend_tokens, "HiCache batch extend token count exceeds uint64 range");
        batch_intent.allocated_pages =
            core::checked_add_u64(batch_intent.allocated_pages, allocation_intent.allocated_pages, "HiCache batch allocated page count exceeds uint64 range");
        batch_intent.requests.push_back(CacheExtendRequestIntent{
            .request_key = key,
            .request_id = entry_fact.request_id,
            .accepted_tokens = resolution.token_count,
            .target_device_prefix_tokens = device_prefix_tokens,
            .prior_committed_prefix_tokens = prior_committed_prefix_tokens,
            .allocation_prefix_tokens = allocation_prefix_tokens,
            .extend_tokens = allocation_intent.extend_tokens,
            .requested_pages = allocation_intent.requested_pages,
            .allocated_pages = allocation_intent.allocated_pages,
            .full_pages = request.full_pages,
            .device_pages = request.device_pages,
            .host_pages = request.host_pages,
        });
    }
    batch_intent.requested_pages = ceil_div(detail::extend_requested_tokens(detail::ExtendPressureInput{
                                                .extend_tokens = batch_intent.total_extend_tokens,
                                                .batch_size = batch_intent.batch_size,
                                                .page_size = page_size,
                                            }),
                                            page_size);

#ifdef DEBUG
    const auto allocator_available_before = scope.device_allocator.available_pages();
#endif
    enforce_device_capacity(fact, summary, transitions, scope, batch_intent.requested_pages);
    scope.device_allocator.merge_before_extend(batch_intent.total_extend_tokens, batch_intent.batch_size, page_size);

    for (const auto & intent : batch_intent.requests) {
        auto it = scope.requests.find(intent.request_key);
        if (it == scope.requests.end()) continue;
        auto & request = it->second;
        const auto owner = request_ref_owner(intent.request_key);
        auto ref = scope.refs.release_owner(scope.tree, owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_ref_release_before_cache_extend");
        const auto consumed_pages = scope.device_allocator.allocate(intent.allocated_pages);
        request.kv_allocated_pages =
            core::checked_add_u64(request.kv_allocated_pages, consumed_pages, "HiCache request allocated page count exceeds uint64 range");
        request.cache_protected_pages = std::max(request.cache_protected_pages, static_cast<uint64_t>(intent.device_pages.size()));
        request.lifecycle_state = "extended";
        ref = scope.refs.acquire_lock(scope.tree, owner, "request", intent.request_key, "", request.device_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_lock_ref_acquire_cache_extend");
        ref = scope.refs.acquire_host(scope.tree, owner, "request", intent.request_key, "", request.host_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_host_ref_acquire_cache_extend");
#ifdef DEBUG
        {
            const auto capacity_snapshot = scope.capacity.snapshot();
            const auto full_missing_pages =
                intent.full_pages.size() > intent.device_pages.size() ? static_cast<uint64_t>(intent.full_pages.size() - intent.device_pages.size()) : 0;
            const auto allocation_pressure_needed = batch_intent.requested_pages > 0;
            auto request_fact = batch_request_fact(fact, intent.request_id, intent.accepted_tokens);
            record_policy_decision(
                request_fact,
                HiCachePolicyDecisionRecord{
                    .policy_area = "device_allocation",
                    .policy_name = "cache_extend_batch_intent",
                    .decision = allocation_pressure_needed ? "reserve_target_extend_budget" : "skip_full_hit_allocation_pressure",
                    .reason = "cache_extend_input uses batch-level accepted fill paths and target device prefix to derive SGLang extend pressure",
                    .accepted = allocation_pressure_needed,
                    .requested_pages = batch_intent.requested_pages,
                    .requested_tokens = detail::extend_requested_tokens(detail::ExtendPressureInput{
                        .extend_tokens = batch_intent.total_extend_tokens,
                        .batch_size = batch_intent.batch_size,
                        .page_size = page_size,
                    }),
                    .candidate_pages = full_missing_pages,
                    .hit_pages = static_cast<uint64_t>(intent.device_pages.size()),
                    .batch_size = batch_intent.batch_size,
                    .accepted_tokens = intent.accepted_tokens,
                    .target_device_prefix_tokens = intent.target_device_prefix_tokens,
                    .prior_committed_prefix_tokens = intent.prior_committed_prefix_tokens,
                    .allocation_prefix_tokens = intent.allocation_prefix_tokens,
                    .extend_tokens = intent.extend_tokens,
                    .allocated_pages = consumed_pages,
                    .active_request_pages = request.kv_allocated_pages,
                    .capacity_pages = policy_.l1_capacity_pages(),
                    .occupied_pages = capacity_snapshot.occupied_device_pages,
                    .reserved_pages = request.kv_allocated_pages,
                    .allocator_free_pages = scope.device_allocator.free_pages,
                    .allocator_release_pages = scope.device_allocator.release_pages,
                    .allocator_available_pages = scope.device_allocator.available_pages(),
                    .allocator_available_before_pages = allocator_available_before,
                    .allocator_consumed_pages = consumed_pages,
                    .pages = request.full_pages,
                });
            record_transition(request_fact,
                              summary,
                              transitions,
                              TransitionDescriptor{ .kind = "cache_extend_acquire_request_ref", .tier = "node_ref" },
                              request.device_pages,
                              debug_state_digest());
        }
#endif
    }

    drain_prefetch_after_cache_extend(*entry_facts, summary, transitions, scope);
}

/**
 * @brief Materializes a committed request path into device radix.
 *
 * Insertion owns device residency and dirty/hit-count visibility. The caller retains
 * write policy, capacity enforcement, and request-tail release at lifecycle boundaries.
 */
HiCacheInsertResult HiCacheState::insert_request_path(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                      ScopedState & scope, const std::vector<std::string> & pages) {
    const auto before = debug_state_digest();
    const auto dirty_visible_at_insert = inserted_device_dirty_visible_at_insert_boundary();
    auto insert = scope.tree.insert_device_path(pages, fact.priority, dirty_visible_at_insert);
    sync_capacity_for_insert(scope, normalized_scope(fact), insert, "request_insert_device");
    if constexpr (debug_records_enabled()) {
        auto new_pages = flatten_node_pages(scope.tree, insert.new_device_nodes);
        auto restored_pages = flatten_node_pages(scope.tree, insert.restored_device_nodes);
        auto dirtied_pages = flatten_node_pages(scope.tree, insert.dirtied_device_nodes);
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "add_l1_residency", .tier = "L1" }, new_pages, before);
        record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "restore_l1_residency", .tier = "L1" }, restored_pages, before);
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
        if (dirty_visible_at_insert)
            record_transition(fact, summary, transitions, TransitionDescriptor{ .kind = "mark_dirty", .tier = "dirty" }, dirtied_pages, before);
    }
    return insert;
}

/**
 * @brief Commits request lifecycle state and releases request-owned KV/references.
 *
 * A finished request releases duplicate radix-covered KV and tail KV. An unfinished
 * request releases only duplicates, retains partial-tail allocator ownership, and
 * reacquires request references for the next cache extend.
 */
void HiCacheState::apply_cache_lifecycle_commit(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions) {
    const auto kind = lower_copy(fact.lifecycle_kind);
    if (!kind.empty() && kind != "finished" && kind != "unfinished") return;

    const auto resolution = token_directory_.resolve_cache_lifecycle_commit_path(fact, pager_.page_size_for_fact(fact));
    record_token_resolution(fact, summary, resolution);
    const auto lifecycle_token_count = resolution.token_count;
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);
    scope.storage.observe_path(page_path);
#ifdef DEBUG
    scope.tree.observe_page_path(page_path);
#endif
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto & request = scope.requests[key];
    const auto protected_pages_before_insert = request.cache_protected_pages;
#ifdef DEBUG
    const auto request_owned_pages_before_insert = request.kv_allocated_pages;
#endif
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
    const auto pages_to_release = core::checked_add_u64(duplicate_pages, tail_release_pages, "HiCache lifecycle release page count exceeds uint64 range");
#ifdef DEBUG
    const auto released_pages = scope.device_allocator.release(pages_to_release);
#else
    (void)scope.device_allocator.release(pages_to_release);
#endif
    it->second.kv_allocated_pages = kind == "unfinished" ? unfinished_tail_pages : uint64_t{ 0 };
    it->second.committed_tokens = lifecycle_token_count;
#ifdef DEBUG
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
#endif
    if (kind == "unfinished") {
        it->second.lifecycle_state = "unfinished";
        it->second.cache_protected_pages = page_aligned_pages;
        const auto owner = request_ref_owner(key);
        auto ref = scope.refs.release_owner(scope.tree, owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_ref_release_before_unfinished");
        ref = scope.refs.acquire_lock(scope.tree, owner, "request", key, "", it->second.device_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_lock_ref_acquire_unfinished");
        ref = scope.refs.acquire_host(scope.tree, owner, "request", key, "", it->second.host_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_host_ref_acquire_unfinished");
    }
    else {
        const auto before = debug_state_digest();
        const auto ref = scope.refs.release_owner(scope.tree, request_ref_owner(key));
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_ref_release_finished");
        if constexpr (debug_records_enabled())
            record_transition(fact,
                              summary,
                              transitions,
                              TransitionDescriptor{ .kind = "release_request_ref", .tier = "node_ref" },
                              it->second.full_pages,
                              before);
        scope.requests.erase(it);
    }
}


} // namespace markov::trace_graph::modules::hicache::model
