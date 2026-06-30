/**
 * @file
 * @brief HiCache request match/admission/lifecycle 的状态推进逻辑。
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
 * @brief 用一次 fact 后的 radix lookup 刷新 request-local 视图。
 *
 * request state 只缓存“这个 request 看到的 path/chain”，方便后续 admission 和
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
 * 这个边界发生在 admission 前；它只能消费已经在 target radix 中可见的 host prefix，
 * 不能根据 storage-readable 事实自行发明 H2D loadback。缺少 scheduler host-hit/loadback
 * intent 时，device capacity 不足会跳过 loadback，而不是主动触发 eviction。
 */
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
    auto lookup = scope.tree.lookup(pages);
    sync_capacity(scope, normalized_scope(fact), lookup.topology_chain, "match_anchor_touch");

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

/**
 * @brief 处理 request admission 边界：重新绑定 request refs，并投影 device allocation pressure。
 *
 * SGLang admission 会基于 prefix 命中结果申请 extend KV page。模型使用 target page
 * path 和 explicit batch_size=1 合同估算 allocator pressure；实际 residency 插入仍在
 * lifecycle anchor 上完成，避免 admission 阶段把尚未提交的 path 写入 canonical tree。
 */
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

    /**
     * @brief terminal prefetch 的 host reservation 不在 revoke/apply 当场释放。
     *
     * 它在后续同 request admission 后 drain；这个 target-derived 边界近似 SGLang
     * 后台队列和 scheduler progress 的交互，同时避免跨配置复用 source scheduler checkpoint。
     */
    uint64_t requested_host_pages = 0;
    uint64_t reserved_host_pages = 0;
    uint64_t released_operation_count = 0;
    std::string released_operation_id;
    std::vector<std::string> planned_pages;
    auto & prefetch_ops = scope.async_ops.prefetch_ops();
    for (const auto & operation_id : scope.async_ops.operations_for_request(key)) {
        auto prefetch = prefetch_ops.find(operation_id);
        if (prefetch == prefetch_ops.end() || prefetch_active(prefetch->second) || prefetch->second.reserved_host_pages == 0) continue;
        requested_host_pages += prefetch->second.requested_host_pages;
        reserved_host_pages += prefetch->second.reserved_host_pages;
        planned_pages.insert(planned_pages.end(), prefetch->second.planned_pages.begin(), prefetch->second.planned_pages.end());
        ++released_operation_count;
        released_operation_id = released_operation_count == 1 ? prefetch->second.header.operation_id : "multiple_prefetch_operations";
    }
    if (reserved_host_pages == 0) return;
    const auto released_pages = scope.async_ops.release_prefetch_pending_host_pages_for_request(key);
    if (released_pages == 0) return;
    sync_capacity(scope, normalized_scope(fact), {}, "prefetch_post_admission_host_release");
    record_policy_decision(fact,
                           HiCachePolicyDecisionRecord{
                               .operation_id = released_operation_id,
                               .policy_area = "host_allocation",
                               .policy_name = "prefetch_host_release_queue",
                               .decision = "drain_request_prefetch_pending_release_pages",
                               .reason = "target-derived post-admission boundary releases terminal prefetch host reservation for the admitted request",
                               .accepted = true,
                               .requested_pages = requested_host_pages,
                               .candidate_pages = static_cast<uint64_t>(planned_pages.size()),
                               .reserved_pages = reserved_host_pages,
                               .allocator_released_pages = released_pages,
                               .pages = planned_pages,
                           });
}

/**
 * @brief 将 committed request path materialize 到 device radix。
 *
 * insert 只维护 device residency 和 dirty/hit_count 可见性；write policy、capacity
 * enforcement 和 request-local tail release 由调用方按 lifecycle 阶段继续处理。
 */
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

/**
 * @brief 处理 request lifecycle anchor，并释放 request-owned KV/ref。
 *
 * finished 会释放 duplicate radix-covered KV 和 tail KV；unfinished 只释放 duplicate，
 * 保留 partial tail 的 allocator ownership，并重新挂回 request ref，供下一轮 admission
 * 继续使用。
 */
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


} // namespace markov::trace_graph::modules::hicache::model
