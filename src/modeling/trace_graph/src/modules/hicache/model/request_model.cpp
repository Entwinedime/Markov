/**
 * @file
 * @brief Advances HiCache lookup, extend, and lifecycle-commit state.
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <stdexcept>
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
    fact.execution_anchor_node_id = batch.execution_anchor_node_id;
    fact.source_event_index = batch.source_event_index;
    fact.source_ts = batch.source_ts;
    fact.ts = batch.ts;
    fact.dur = batch.dur;
    fact.pid = batch.pid;
    fact.tid = batch.tid;
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

std::vector<HiCacheNodeId> loadback_promoted_nodes(const HiCacheTokenRadixTree & tree, const radix::HiCachePathLookup & lookup, size_t device_prefix_pages,
                                                   size_t promotable_prefix_pages) {
    /**
     * @brief Mirrors HiRadixCache.load_back's walk from the deepest host hit.
     *
     * SGLang promotes every consecutive host-backed, device-evicted radix node after
     * the device prefix.  `lookup.host_chain` cannot represent that suffix because it
     * intentionally describes a host prefix starting at the root.
     */
    std::vector<HiCacheNodeId> nodes;
    size_t page_offset = 0;
    for (const auto node_id : lookup.topology_chain) {
        const auto * node = tree.node(node_id);
        if (node == nullptr) continue;
        const auto node_begin = page_offset;
        const auto node_end = page_offset + node->pages.size();
        page_offset = node_end;
        if (node_end <= device_prefix_pages) continue;
        if (node_begin >= promotable_prefix_pages) break;
        if (node_begin < device_prefix_pages || node_end > promotable_prefix_pages) continue;
        if (!node->residency.device_present && node->residency.host_present && node->residency.host_visible) nodes.push_back(node_id);
    }
    return nodes;
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
    auto & request = scope.requests[key];
    auto lookup_pages = pages;
    if (fact.role == "cache_extend_input" && request.lookup_token_limit) {
        const auto limit_pages = *request.lookup_token_limit / pager_.page_size_for_fact(fact);
        lookup_pages.resize(std::min<uint64_t>(lookup_pages.size(), limit_pages));
    }
    auto lookup = scope.tree.lookup_peek(lookup_pages);
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
 * in target radix. The queue-time lookup before the prefetch candidate is discovery-only;
 * a later admission lookup may initiate H2D loadback. Once eligible, a host hit is
 * sufficient loadback intent and SGLang retries failed allocation after radix eviction.
 */
void HiCacheState::apply_cache_lookup_input(const HiCacheFact & fact) {
    auto & scope = scope_state(fact);
    const auto request_key = scoped_request_key(fact);
    if (auto * prefetch = scope.async_ops.prefetch_for_request(request_key); prefetch != nullptr && prefetch_active(*prefetch)) {
        if (const auto * boundary = prefetch_control_boundary_for_lookup(fact); boundary != nullptr)
            settle_prefetch_before_cache_extend(*boundary, scope, request_key);
    }

    const auto resolution = runtime::resolve_cache_lookup_path(fact);
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (!request_key.empty() && resolution.ok()) scope.requests[request_key].lookup_token_limit = resolution.token_count;
    if (pages.empty()) return;

    ensure_device_allocator(scope);
    scope.storage.observe_path(page_path);
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain);

    const auto request = scope.requests.find(request_key);
    const bool loadback_eligible = request != scope.requests.end() && request->second.prefetch_candidate_seen;
    /**
     * @brief Extends the device prefix through an adjacent host-visible suffix.
     *
     * A device-only prefix followed by host-visible pages is one contiguous reusable
     * memory prefix. A host-only lookup stops at the first device-only node and would
     * therefore hide the suffix that loadback must promote. Storage readability alone
     * remains insufficient to materialize L1 residency.
     */
    const auto promotable_pages = scope.tree.contiguous_prefix(pages, true, true, false);
    if (loadback_eligible && promotable_pages.size() > lookup.device_pages.size()) {
        const auto loadback_pages = static_cast<uint64_t>(promotable_pages.size() - lookup.device_pages.size());
        const auto promoted_nodes = loadback_promoted_nodes(scope.tree, lookup, lookup.device_pages.size(), promotable_pages.size());
        if (execution_prefetch_control_) {
            auto & work = scope.load_admission_work[fact.source_node_id];
            work = HiCacheLoadAdmissionWork{ .requested_pages = loadback_pages };
            for (const auto node : promoted_nodes) work.node_pages.push_back(scope.tree.node(node)->pages.size());
        }
        const auto allocation_owner = request_key + ":loadback_allocation";
        auto ref = scope.refs.acquire_lock(scope.tree, allocation_owner, lookup.device_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        ref = scope.refs.acquire_host(scope.tree, allocation_owner, promoted_nodes);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        scope.device_allocator.merge_before_page_allocation(loadback_pages);
        const auto budget = scope.device_allocator.can_allocate(loadback_pages) ? 0 : loadback_pages;
        auto finish = [this, &scope, request_key, allocation_owner, loadback_pages, pages, lookup, promotable_pages, promoted_nodes](const HiCacheFact & fact) {
            HiCacheRefChange ref;
            scope.device_allocator.merge_before_page_allocation(loadback_pages);
            if (!scope.device_allocator.can_allocate(loadback_pages)) {
                ref = scope.refs.release_owner(scope.tree, allocation_owner);
                sync_capacity_for_ref(scope, normalized_scope(fact), ref);
                update_request_state(fact, scope, pages);
                return;
            }
            const auto consumed_pages = scope.device_allocator.allocate(loadback_pages);
            if (consumed_pages != loadback_pages) {
                ref = scope.refs.release_owner(scope.tree, allocation_owner);
                sync_capacity_for_ref(scope, normalized_scope(fact), ref);
                update_request_state(fact, scope, pages);
                return;
            }

            const auto loadback_id = scope.clock.next_operation_id("loadback");
            if (execution_prefetch_control_) scope.load_admission_work.at(fact.source_node_id).allocated = true;
            const auto loadback_owner = request_key + ":loadback:" + loadback_id;
            const auto transferred_pages = suffix_from(promotable_pages, lookup.device_pages.size());
            auto loadback_header = make_operation_header(loadback_id, fact, normalized_scope(fact), request_key, loadback_owner, transferred_pages, 0);
            // Admission may have settled this prefetch at a later predicted control
            // boundary. Logical host residency does not permit H2D before publication.
            // Keep the lookup fact as provenance; advance only the actual I/O enqueue.
            if (const auto * prefetch = scope.async_ops.prefetch_for_request(request_key); prefetch != nullptr) {
                const bool consumes_prefetch = std::ranges::any_of(transferred_pages, [&](const auto & page) {
                    return std::ranges::find(prefetch->completed_pages, page) != prefetch->completed_pages.end();
                });
                if (consumes_prefetch) loadback_header.enqueue_ts = std::max(loadback_header.enqueue_ts, prefetch->target_boundary_ts);
            }
            const auto io_schedule = execution_prefetch_control_
                                         ? HiCacheIoSchedule{}
                                         : schedule_target_io(normalized_scope(fact), "load", loadback_header.enqueue_ts, transferred_pages.size());
            scope.async_ops.insert_loadback(HiCacheLoadbackOperation{
                .header = std::move(loadback_header),
                .io_schedule = io_schedule,
            });
            ref = scope.refs.acquire_lock(scope.tree, loadback_owner, lookup.topology_chain);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref);
            if (execution_prefetch_control_) {
                ref = scope.refs.acquire_host(scope.tree, loadback_owner, promoted_nodes);
                sync_capacity_for_ref(scope, normalized_scope(fact), ref);
            }
            ref = scope.refs.release_owner(scope.tree, allocation_owner);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref);
            auto insert = scope.tree.insert_device_path(promotable_pages, fact.priority, false);
            sync_capacity_for_insert(scope, normalized_scope(fact), insert);
            if (execution_prefetch_control_) scope.pending_loadbacks.push_back(loadback_id);
            else {
                const auto completion_ts = io_schedule.available ? io_schedule.ready_ts : fact.ts;
                scope.async_ops.set_loadback_state(loadback_id, HiCacheOperationState::Committed, completion_ts);
                ref = scope.refs.release_owner(scope.tree, loadback_owner);
                sync_capacity_for_ref(scope, normalized_scope(fact), ref);
            }
            update_request_state(fact, scope, pages);
            auto & promoted_request = scope.requests[request_key];
            const auto owner = request_ref_owner(request_key);
            ref = scope.refs.release_owner(scope.tree, owner);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref);
            ref = scope.refs.acquire_lock(scope.tree, owner, promoted_request.device_chain);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        };
        allocate_after_capacity(fact, scope, budget, std::move(finish));
        return;
    }
    update_request_state(fact, scope, pages);
}

/**
 * @brief Resolves every accepted cache-extend path from the current batch fact.
 *
 * Resolution is deliberately fact-local. A missing or inconsistent entry aborts the
 * complete batch so request history cannot fill a partial target input.
 */
std::optional<std::vector<HiCacheFact>> HiCacheState::resolve_cache_extend_entry_facts(const HiCacheFact & fact,
                                                                                       const HiCacheBatchTokenResolution & batch_resolution) {
    if (!batch_resolution.ok()) {
        return std::nullopt;
    }

    std::vector<HiCacheFact> entry_facts;
    entry_facts.reserve(fact.batch_paths.size());
    for (size_t index = 0; index < fact.batch_paths.size(); ++index) {
        const auto & entry = fact.batch_paths[index];
        auto entry_fact = batch_request_fact(fact, entry.request_id, entry.token_count);
        const auto & resolution = batch_resolution.entries[index];
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
void HiCacheState::prepare_prefetch_before_cache_extend(const std::vector<HiCacheFact> & entry_facts, ScopedState & scope) {
    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (!key.empty()) {
            if (auto * loadback = scope.async_ops.loadback_for_request(key); loadback != nullptr && loadback->header.consumer_epoch == 0) {
                const auto consumer_epoch = scope.clock.next_boundary_epoch();
                loadback->header.consumer_epoch = consumer_epoch;
                loadback->header.consumer_ts = entry_fact.ts;
                loadback->header.consumer_source_node_id = entry_fact.source_node_id;
                loadback->header.consumer_execution_anchor_node_id = entry_fact.execution_anchor_node_id;
                loadback->header.consumer_source_event_index = entry_fact.source_event_index;
                loadback->header.consumer_source_fact_role = entry_fact.role;
                loadback->header.consumer_source_available = true;
            }
            // Drain prior terminal prefetches before scheduling this extend.
            drain_prefetch_pending_release(entry_fact, scope, key);
        }
    }
    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (!key.empty()) settle_prefetch_before_cache_extend(entry_fact, scope, key);
    }
}

/** @brief Drains releases created by the current cache-extend settlement round. */
void HiCacheState::drain_prefetch_after_cache_extend(const std::vector<HiCacheFact> & entry_facts, ScopedState & scope) {
    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (!key.empty()) drain_prefetch_pending_release(entry_fact, scope, key);
    }
}

/**
 * @brief Rebinds request references and projects batch device-allocation pressure.
 *
 * The start of `ScheduleBatch.prepare_for_extend` has formed accepted fill paths but
 * has not run allocation. This handler computes pressure for the complete batch;
 * lifecycle commit performs radix insertion so uncommitted paths never become canonical.
 */
void HiCacheState::apply_cache_extend_input(const HiCacheFact & fact) {
    const auto page_size = pager_.page_size_for_fact(fact);
    const auto batch_resolution = runtime::resolve_cache_extend_paths(fact);
    const auto entry_facts = resolve_cache_extend_entry_facts(fact, batch_resolution);
    if (!entry_facts) return;

    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);
    prepare_prefetch_before_cache_extend(*entry_facts, scope);

    CacheExtendBatchIntent batch_intent;
    batch_intent.batch_size = static_cast<uint64_t>(entry_facts->size());
    for (size_t index = 0; index < entry_facts->size(); ++index) {
        const auto & entry_fact = (*entry_facts)[index];
        const auto & resolution = batch_resolution.entries[index];
        auto page_path = page_path_from_resolution(entry_fact, resolution);
        auto pages = page_path.page_ids();
        if (pages.empty()) continue;

        scope.storage.observe_path(page_path);
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

    // The scheduler owns each accepted prefix before it asks the allocator to
    // make room for extension.  Refresh the canonical node chain after prefetch
    // and host cleanup, then hold that chain across the device-eviction pass.
    for (const auto & intent : batch_intent.requests) {
        auto request_fact = batch_request_fact(fact, intent.request_id, intent.accepted_tokens);
        update_request_state(request_fact, scope, intent.full_pages);
        auto it = scope.requests.find(intent.request_key);
        if (it == scope.requests.end()) continue;
        const auto owner = request_ref_owner(intent.request_key);
        auto ref = scope.refs.release_owner(scope.tree, owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        ref = scope.refs.acquire_lock(scope.tree, owner, it->second.device_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        ref = scope.refs.acquire_host(scope.tree, owner, it->second.host_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    }

    const auto budget = batch_intent.requested_pages;
    auto finish = [this, &scope, batch_intent = std::move(batch_intent), entry_facts = *entry_facts, page_size](const HiCacheFact & fact) {
        scope.device_allocator.merge_before_extend(batch_intent.total_extend_tokens, batch_intent.batch_size, page_size);
        if (!scope.device_allocator.can_allocate(batch_intent.allocated_pages)) throw std::runtime_error("Target extend allocation is out of device pages");

        HiCacheAllocatorWorkItem allocation{
            .source_fact_id = fact.source_node_id,
            .source_event_index = fact.source_event_index,
            .pid = fact.pid,
            .formal = formal_window_active_,
            .page_size = page_size,
            .batch_size = batch_intent.batch_size,
            .extend_tokens = batch_intent.total_extend_tokens,
            .allocated_pages = batch_intent.allocated_pages,
            .free_index_offset = scope.device_allocator.free_index_offset,
        };
        for (const auto & intent : batch_intent.requests) allocation.request_ids.push_back(intent.request_id);
        allocator_work_items_.push_back(std::move(allocation));

        if (formal_window_active_) {
            for (size_t index = 0; index < batch_intent.requests.size(); ++index) {
                const auto & intent = batch_intent.requests[index];
                prefill_work_items_.push_back(HiCachePrefillWorkItem{
                    .source_fact_id = fact.source_node_id,
                    .source_event_index = fact.source_event_index,
                    .pid = fact.pid,
                    .request_id = intent.request_id,
                    .batch_position = static_cast<uint64_t>(index),
                    .batch_size = batch_intent.batch_size,
                    .target_page_size = page_size,
                    .prompt_token_count = intent.accepted_tokens,
                    .reusable_prefix_token_count = intent.allocation_prefix_tokens,
                    .prefill_token_count = intent.extend_tokens,
                });
            }
        }

        for (const auto & intent : batch_intent.requests) {
            auto it = scope.requests.find(intent.request_key);
            if (it == scope.requests.end()) continue;
            auto & request = it->second;
            const auto owner = request_ref_owner(intent.request_key);
            auto ref = scope.refs.release_owner(scope.tree, owner);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref);
            const auto consumed_pages = scope.device_allocator.allocate(intent.allocated_pages);
            request.kv_allocated_pages =
                core::checked_add_u64(request.kv_allocated_pages, consumed_pages, "HiCache request allocated page count exceeds uint64 range");
            request.extended_tokens = intent.accepted_tokens;
            request.decoded_tokens = 0;
            request.cache_protected_pages = std::max(request.cache_protected_pages, static_cast<uint64_t>(intent.device_pages.size()));
            request.lifecycle_state = "extended";
            ref = scope.refs.acquire_lock(scope.tree, owner, request.device_chain);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref);
            ref = scope.refs.acquire_host(scope.tree, owner, request.host_chain);
            sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        }

        drain_prefetch_after_cache_extend(entry_facts, scope);
    };
    allocate_after_capacity(fact, scope, budget, std::move(finish));
}

/**
 * @brief Commits request lifecycle state and releases request-owned KV/references.
 *
 * A finished request releases duplicate radix-covered KV and tail KV. An unfinished
 * request releases only duplicates, retains partial-tail allocator ownership, and
 * reacquires request references for the next cache extend.
 */
void HiCacheState::apply_cache_lifecycle_commit(const HiCacheFact & fact, HiCacheLifecycleExecution execution) {
    const auto kind = lower_copy(fact.lifecycle_kind);
    if (!kind.empty() && kind != "finished" && kind != "unfinished") return;
    if (kind == "unfinished" && policy_.write_count_enabled() && !fact.chunked.has_value())
        throw std::runtime_error("Unfinished cache insertion lacks chunked observation for target write policy");

    const auto resolution = runtime::resolve_cache_lifecycle_commit_path(fact);
    const auto lifecycle_token_count = resolution.token_count;
    const auto page_path = page_path_from_resolution(fact, resolution);
    const auto pages = page_path.page_ids();
    if (pages.empty()) return;
    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);
    scope.storage.observe_path(page_path);
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto & request = scope.requests[key];
    const auto protected_pages_before_insert = request.cache_protected_pages;
    const auto allocated_tokens = core::checked_add_u64(request.extended_tokens, request.decoded_tokens, "HiCache allocated token count overflow");
    const auto extended_pages = ceil_div(allocated_tokens, page_path.page_size);
    const auto total_committed_pages = ceil_div(lifecycle_token_count, page_path.page_size);
    const auto decode_allocated_pages = bounded_subtract(total_committed_pages, extended_pages);
    if (execution_prefetch_control_ && request.extended_tokens && decode_allocated_pages)
        throw std::runtime_error("Lifecycle reached unobserved decode page allocations");
    uint64_t consumed_decode_pages = 0;
    for (uint64_t page = 0; page < decode_allocated_pages; ++page) {
        enforce_device_capacity(fact, scope, 1);
        scope.device_allocator.merge_before_page_allocation(1);
        consumed_decode_pages =
            core::checked_add_u64(consumed_decode_pages, scope.device_allocator.allocate(1), "HiCache decode allocated page count exceeds uint64 range");
    }
    request.kv_allocated_pages =
        core::checked_add_u64(request.kv_allocated_pages, consumed_decode_pages, "HiCache request decode page count exceeds uint64 range");
    scope.lifecycle_return = PendingLifecycleReturn{
        fact.source_node_id, key, kind, 0, protected_pages_before_insert,
        extended_pages, total_committed_pages, static_cast<uint64_t>(pages.size()), lifecycle_token_count,
        pages, page_path.page_size, fact.ts };
    if (execution != HiCacheLifecycleExecution::Stepped) advance_lifecycle_insert(fact, lifecycle_token_count);
    if (execution == HiCacheLifecycleExecution::Immediate) complete_lifecycle_return(fact);
}

bool HiCacheState::lifecycle_return_pending(const HiCacheFact & fact) const {
    const auto scope = scopes_.find(normalized_scope(fact));
    return scope != scopes_.end() && scope->second.lifecycle_return && scope->second.lifecycle_return->fact_id == fact.source_node_id;
}

void HiCacheState::advance_lifecycle_insert(const HiCacheFact & fact, uint64_t ready_tokens) {
    if (!lifecycle_return_pending(fact)) throw std::logic_error("Lifecycle insertion does not match its entry");
    auto & scope = scope_state(fact);
    auto & pending = *scope.lifecycle_return;
    if (fact.ts < pending.last_step_us || ready_tokens < pending.ready_tokens || ready_tokens > pending.token_count)
        throw std::logic_error("Lifecycle insertion must advance within its input path and clock");
    pending.last_step_us = fact.ts;
    pending.ready_tokens = ready_tokens;
    const auto ready_pages = std::min<uint64_t>(ready_tokens / pending.page_size, pending.pages.size());
    while (const auto step =
               scope.tree.insert_device_step(pending.pages, fact.priority, inserted_device_dirty_visible_at_insert_boundary(), pending.cursor, ready_pages)) {
        sync_capacity_for_insert(scope, normalized_scope(fact), *step);
        apply_write_count_policy(fact, scope, *step);
    }
    pending.existing_prefix_pages = pending.cursor.result.existing_device_prefix_pages;
    if (pending.cursor.offset == pending.pages.size()) update_request_state(fact, scope, pending.pages);
}

void HiCacheState::complete_lifecycle_return(const HiCacheFact & fact) {
    if (!lifecycle_return_pending(fact)) throw std::logic_error("Lifecycle return does not match its insertion");
    auto & scope = scope_state(fact);
    const auto pending = *scope.lifecycle_return;
    if (pending.cursor.offset != pending.pages.size() || fact.ts < pending.last_step_us)
        throw std::logic_error("Lifecycle return precedes insertion completion");
    const auto & key = pending.request_key;
    const auto & kind = pending.kind;
    auto it = scope.requests.find(key);
    if (it == scope.requests.end()) throw std::logic_error("Lifecycle request disappeared before return");
    const auto duplicate_pages = std::min(bounded_subtract(pending.existing_prefix_pages, pending.protected_pages), it->second.kv_allocated_pages);
    const auto owned_after_duplicate = bounded_subtract(it->second.kv_allocated_pages, duplicate_pages);
    const auto page_aligned_pages = pending.aligned_pages;
    // Overlap may already have allocated the next decode before the previous
    // prefill result publishes its prompt. That in-flight tail is not part of
    // this commit, but it still occupies request-owned device pages.
    const auto tail_pages = bounded_subtract(std::max(pending.committed_pages, pending.extended_pages), page_aligned_pages);
    const auto unfinished_tail_pages = std::min(tail_pages, owned_after_duplicate);
    const auto tail_release_pages = kind == "finished" || kind.empty() ? unfinished_tail_pages : uint64_t{ 0 };
    const auto pages_to_release = core::checked_add_u64(duplicate_pages, tail_release_pages, "HiCache lifecycle release page count exceeds uint64 range");
    (void)scope.device_allocator.release(pages_to_release);
    it->second.kv_allocated_pages = kind == "unfinished" ? unfinished_tail_pages : uint64_t{ 0 };
    it->second.committed_tokens = pending.token_count;
    sync_capacity(scope, normalized_scope(fact), {});
    uint64_t request_owned_pages = 0;
    for (const auto & active_request : scope.requests | std::views::values) {
        request_owned_pages =
            core::checked_add_u64(request_owned_pages, active_request.kv_allocated_pages, "HiCache active request ownership exceeds uint64 range");
    }
    scope.device_allocator.reconcile_occupied_pages(scope.capacity.snapshot().occupied_device_pages, request_owned_pages);
    if (kind == "unfinished") {
        it->second.lifecycle_state = "unfinished";
        it->second.cache_protected_pages = page_aligned_pages;
        const auto owner = request_ref_owner(key);
        auto ref = scope.refs.release_owner(scope.tree, owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        ref = scope.refs.acquire_lock(scope.tree, owner, it->second.device_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        ref = scope.refs.acquire_host(scope.tree, owner, it->second.host_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
    }
    else {
        const auto ref = scope.refs.release_owner(scope.tree, request_ref_owner(key));
        sync_capacity_for_ref(scope, normalized_scope(fact), ref);
        scope.requests.erase(it);
    }
    scope.lifecycle_return.reset();
}


} // namespace markov::trace_graph::modules::hicache::model
