/**
 * @file
 * @brief HiCache cache lookup/extend/lifecycle commit 的状态推进逻辑。
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <utility>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

/**
 * @brief 用一次 fact 后的 radix lookup 刷新 request-local 视图。
 *
 * request state 只缓存“这个 request 看到的 path/chain”，方便后续 cache extend 和
 * lifecycle release 复用；真正的 residency/ref 状态仍以 canonical radix tree 为准。
 */
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

/**
 * @brief 处理 request match 边界，并在安全时把 host-visible prefix load back 到 L1。
 *
 * 这个边界发生在 cache extend 前；它只能消费已经在 target radix 中可见的 host prefix，
 * 不能根据 storage-readable 事实自行发明 H2D loadback。缺少 scheduler host-hit/loadback
 * intent 时，device capacity 不足会跳过 loadback，而不是主动触发 eviction。
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
    scope.tree.observe_page_path(page_path);
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "cache_lookup_touch");

    const auto request_key = scoped_request_key(fact);
    /**
     * @brief modeled loadback 只把 host-visible prefix 同步 materialize 到 L1。
     *
     * storage-readable 只说明 L3 可读，不等价于本轮已经完成 H2D loadback；这里必须要求
     * radix 上已经有 host-visible residency。
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
        const auto before_enqueue = debug_state_digest();
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
        if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "enqueue_loadback", "loadback", promotable_pages, before_enqueue);
        const auto before = debug_state_digest();
        auto insert = scope.tree.insert_device_path(promotable_pages, fact.priority, false);
        sync_capacity_for_insert(scope, normalized_scope(fact), insert, "loadback_insert_device");
        if constexpr (debug_records_enabled())
            record_transition(fact,
                              summary,
                              transitions,
                              "promote_visible_prefix_to_l1",
                              "L1",
                              flatten_node_pages(scope.tree, insert.restored_device_nodes),
                              before);
        const auto before_complete = debug_state_digest();
        scope.async_ops.set_loadback_state(loadback_id, HiCacheOperationState::Committed, "sync_commit", fact.ts);
        ref = scope.refs.release_owner(scope.tree, loadback_owner);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "loadback_ref_release");
        if constexpr (debug_records_enabled())
            record_transition(fact, summary, transitions, "complete_loadback", "loadback", promotable_pages, before_complete);
    }

    update_request_state(fact, scope, pages);
}

/**
 * @brief 处理 batch-level cache extend 输入：重新绑定 request refs，并投影 device allocation pressure。
 *
 * `ScheduleBatch.prepare_for_extend` start phase 已经形成 batch 内 accepted fill path，
 * 但 allocator 尚未执行。模型在这里按 batch 统一计算 extend pressure；实际 radix
 * residency 插入仍在 lifecycle commit 上完成，避免 extend 阶段把尚未提交的 path
 * 写入 canonical tree。
 */
void HiCacheState::apply_cache_extend_input(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions) {
    const auto page_size = pager_.page_size_for_fact(fact);
    const auto batch_resolution = token_directory_.resolve_cache_extend_paths(fact, page_size);
    if (!batch_resolution.ok()) {
#ifdef DEBUG
        summary.missing_state_model_facts["cache_extend_batch_token_resolution_" + hicache_token_resolution_status_name(batch_resolution.status)]++;
#else
        (void)summary;
#endif
        return;
    }

    auto & scope = scope_state(fact);
    ensure_device_allocator(scope);

    std::vector<HiCacheFact> entry_facts;
    std::vector<HiCacheTokenResolution> entry_resolutions;
    entry_facts.reserve(fact.batch_paths.size());
    entry_resolutions.reserve(batch_resolution.entries.size());
    for (size_t index = 0; index < fact.batch_paths.size(); ++index) {
        const auto & entry = fact.batch_paths[index];
        HiCacheFact entry_fact = fact;
        entry_fact.request_id = entry.request_id;
        entry_fact.full_path_span = entry.full_path_span;
        entry_fact.full_path_tokens = entry.full_path_tokens;
        entry_fact.token_count = entry.token_count;
        const auto & resolution = batch_resolution.entries[index];
        record_token_resolution(entry_fact, summary, resolution);
        if (!resolution.ok()) return;
        entry_facts.push_back(std::move(entry_fact));
        entry_resolutions.push_back(resolution);
    }

    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (!key.empty()) settle_prefetch_before_cache_extend(entry_fact, summary, transitions, scope, key);
    }

    CacheExtendBatchIntent batch_intent;
    batch_intent.batch_size = static_cast<uint64_t>(entry_facts.size());
    for (size_t index = 0; index < entry_facts.size(); ++index) {
        const auto & entry_fact = entry_facts[index];
        const auto & resolution = entry_resolutions[index];
        auto page_path = page_path_from_resolution(entry_fact, resolution);
        auto pages = page_path.page_ids();
        if (pages.empty()) continue;

        scope.storage.observe_path(page_path);
        scope.tree.observe_page_path(page_path);
        update_request_state(entry_fact, scope, pages);

        const auto key = scoped_request_key(entry_fact);
        if (key.empty()) { continue; }
        auto & request = scope.requests[key];
        const auto device_prefix_pages = static_cast<uint64_t>(request.device_pages.size());
        const auto device_prefix_tokens = device_prefix_pages * page_path.page_size;
        const auto prior_committed_prefix_tokens =
            request.lifecycle_state == "unfinished" ? std::min(request.committed_tokens, resolution.token_count) : uint64_t{ 0 };
        const auto allocation_prefix_tokens = std::max(device_prefix_tokens, prior_committed_prefix_tokens);
        auto allocation_intent = make_extend_allocation_intent(resolution.token_count, allocation_prefix_tokens, page_path.page_size, 1);
        batch_intent.total_extend_tokens += allocation_intent.extend_tokens;
        batch_intent.allocated_pages += allocation_intent.allocated_pages;
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
    batch_intent.requested_pages = ceil_div(detail::extend_requested_tokens(batch_intent.total_extend_tokens, batch_intent.batch_size, page_size), page_size);

    const auto allocator_available_before = scope.device_allocator.available_pages();
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
        request.kv_allocated_pages += consumed_pages;
        request.cache_protected_pages = std::max(request.cache_protected_pages, static_cast<uint64_t>(intent.device_pages.size()));
        request.page_aligned_key_pages = static_cast<uint64_t>(intent.full_pages.size());
        request.active_request_pages = request.kv_allocated_pages;
        request.lifecycle_state = "extended";
        ref = scope.refs.acquire_lock(scope.tree, owner, "request", intent.request_key, "", request.device_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_lock_ref_acquire_cache_extend");
        ref = scope.refs.acquire_host(scope.tree, owner, "request", intent.request_key, "", request.host_chain);
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_host_ref_acquire_cache_extend");
        const auto capacity_snapshot = scope.capacity.snapshot();
        HiCacheFact request_fact = fact;
        request_fact.request_id = intent.request_id;
        const auto full_missing_pages =
            intent.full_pages.size() > intent.device_pages.size() ? static_cast<uint64_t>(intent.full_pages.size() - intent.device_pages.size()) : 0;
        const auto allocation_pressure_needed = batch_intent.requested_pages > 0;
        if constexpr (debug_records_enabled())
            record_policy_decision(
                request_fact,
                HiCachePolicyDecisionRecord{
                    .policy_area = "device_allocation",
                    .policy_name = "cache_extend_batch_intent",
                    .decision = allocation_pressure_needed ? "reserve_target_extend_budget" : "skip_full_hit_allocation_pressure",
                    .reason = "cache_extend_input uses batch-level accepted fill paths and target device prefix to derive SGLang extend pressure",
                    .accepted = allocation_pressure_needed,
                    .requested_pages = batch_intent.requested_pages,
                    .requested_tokens = detail::extend_requested_tokens(batch_intent.total_extend_tokens, batch_intent.batch_size, page_size),
                    .candidate_pages = full_missing_pages,
                    .hit_pages = static_cast<uint64_t>(intent.device_pages.size()),
                    .batch_size = batch_intent.batch_size,
                    .accepted_tokens = intent.accepted_tokens,
                    .target_device_prefix_tokens = intent.target_device_prefix_tokens,
                    .prior_committed_prefix_tokens = intent.prior_committed_prefix_tokens,
                    .allocation_prefix_tokens = intent.allocation_prefix_tokens,
                    .extend_tokens = intent.extend_tokens,
                    .allocated_pages = consumed_pages,
                    .active_request_pages = request.active_request_pages,
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
        if constexpr (debug_records_enabled())
            record_transition(request_fact, summary, transitions, "cache_extend_acquire_request_ref", "node_ref", request.device_pages, debug_state_digest());
    }

    for (const auto & entry_fact : entry_facts) {
        const auto key = scoped_request_key(entry_fact);
        if (!key.empty()) drain_prefetch_release_after_cache_extend(entry_fact, summary, transitions, scope, key);
    }
}

/**
 * @brief 将 committed request path materialize 到 device radix。
 *
 * insert 只维护 device residency 和 dirty/hit_count 可见性；write policy、capacity
 * enforcement 和 request-local tail release 由调用方按 lifecycle 阶段继续处理。
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
    }
    return insert;
}

/**
 * @brief 处理 request lifecycle commit，并释放 request-owned KV/ref。
 *
 * finished 会释放 duplicate radix-covered KV 和 tail KV；unfinished 只释放 duplicate，
 * 保留 partial tail 的 allocator ownership，并重新挂回 request ref，供下一轮 cache extend
 * 继续使用。
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
    if constexpr (debug_records_enabled())
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
        const auto before = debug_state_digest();
        const auto ref = scope.refs.release_owner(scope.tree, request_ref_owner(key));
        sync_capacity_for_ref(scope, normalized_scope(fact), ref, "request_ref_release_finished");
        if constexpr (debug_records_enabled()) record_transition(fact, summary, transitions, "release_request_ref", "node_ref", it->second.full_pages, before);
        scope.requests.erase(it);
    }
}


} // namespace markov::trace_graph::modules::hicache::model
