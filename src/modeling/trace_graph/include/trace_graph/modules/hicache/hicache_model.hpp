#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_async_state.hpp"
#include "trace_graph/modules/hicache/hicache_capacity_index.hpp"
#include "trace_graph/modules/hicache/hicache_fact.hpp"
#include "trace_graph/modules/hicache/hicache_policy.hpp"
#include "trace_graph/modules/hicache/hicache_ref_ledger.hpp"
#include "trace_graph/modules/hicache/hicache_router.hpp"
#include "trace_graph/modules/hicache/hicache_state_index.hpp"
#include "trace_graph/modules/hicache/hicache_storage_directory.hpp"
#include "trace_graph/modules/hicache/hicache_summary.hpp"
#include "trace_graph/modules/hicache/hicache_target_control_clock.hpp"
#include "trace_graph/modules/hicache/hicache_target_pager.hpp"
#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"
#include "trace_graph/modules/hicache/hicache_token_store.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

class DagGraph;

/**
 * @brief HiCache target state 的 canonical-radix 建模器。
 *
 * 每个 cache_scope 维护一棵 canonical radix tree，node residency/ref 是唯一状态源；
 * L1/L2/L3、dirty、backuped、evicted、locked 和 prefetch lifecycle 都从 tree/async
 * table 派生到 summary。
 */
class HiCacheState {
public:
    explicit HiCacheState(HiCacheConfig config = HiCacheConfig{});

    [[nodiscard]] std::vector<HiCacheStateTransition> apply_fact(const HiCacheFact & fact, HiCacheFactRole role, HiCacheSummary & summary);
    [[nodiscard]] std::vector<HiCacheStateTransition> finalize(HiCacheSummary & summary);
    [[nodiscard]] HiCacheDerivedStateSnapshot derived_state(HiCacheDerivedStateMode mode = HiCacheDerivedStateMode::MaterializedOnly) const;
    [[nodiscard]] uint64_t active_ref_owner_count() const;
    [[nodiscard]] uint64_t radix_split_count() const;
    [[nodiscard]] std::vector<HiCacheNodeSplitRecord> radix_split_trace() const;
    [[nodiscard]] uint64_t control_checkpoint_count() const;
    [[nodiscard]] std::vector<HiCacheControlCheckpoint> control_checkpoint_trace() const;
    [[nodiscard]] uint64_t async_lifecycle_transition_count() const;
    [[nodiscard]] std::vector<HiCacheOperationLifecycleTransition> async_lifecycle_trace() const;
    [[nodiscard]] uint64_t policy_decision_count() const;
    [[nodiscard]] const std::vector<HiCachePolicyDecisionRecord> & policy_decision_trace() const { return policy_decisions_; }
    [[nodiscard]] uint64_t storage_known_page_count() const;
    [[nodiscard]] uint64_t storage_readable_page_count() const;
    [[nodiscard]] uint64_t storage_backend_readable_count() const;
    [[nodiscard]] uint64_t storage_materialized_page_count() const;
    [[nodiscard]] uint64_t capacity_mutation_count() const;
    [[nodiscard]] uint64_t capacity_victim_choice_count() const;
    [[nodiscard]] std::vector<HiCacheCapacityMutation> capacity_mutation_trace() const;
    [[nodiscard]] std::vector<HiCacheCapacityVictimChoice> capacity_victim_choices() const;
    [[nodiscard]] std::vector<HiCacheCapacityAuditIssue> capacity_audit_issues() const;
    [[nodiscard]] uint64_t ref_mutation_count() const;
    [[nodiscard]] std::vector<HiCacheRefMutation> ref_mutation_trace() const;
    [[nodiscard]] std::vector<HiCacheRefAuditIssue> ref_audit_issues() const;
    [[nodiscard]] std::string digest() const;

private:
    struct RequestState {
        std::string request_key;
        std::string cache_scope;
        uint64_t committed_tokens = 0;
        uint64_t kv_allocated_pages = 0;
        uint64_t cache_protected_pages = 0;
        uint64_t page_aligned_key_pages = 0;
        uint64_t active_request_pages = 0;
        std::vector<std::string> full_pages;
        std::vector<std::string> device_pages;
        std::vector<std::string> host_pages;
        std::vector<HiCacheNodeId> device_chain;
        std::vector<HiCacheNodeId> host_chain;
        std::string lifecycle_state;
    };

    /** @brief 等待下一轮 target control 边界 drain 的 write-through backup lock。 */
    struct PendingWriteThroughBackup {
        std::string owner;
        std::vector<std::string> pages;
    };

    /**
     * @brief SGLang device KV allocator 的 count-level 投影。
     *
     * 该账本只维护 `free_pages` / `release_pages` 计数，用于还原
     * `allocator.available_size()` gate；逻辑 residency 仍由 radix tree 维护。
     */
    struct DeviceAllocatorLedger {
        bool initialized = false;
        bool need_sort = false;
        uint64_t capacity_pages = 0;
        uint64_t free_pages = 0;
        uint64_t release_pages = 0;

        void configure(uint64_t pages, bool sort_required);
        [[nodiscard]] uint64_t available_pages() const;
        [[nodiscard]] bool should_evict(uint64_t requested_pages) const;
        void merge_release_pages();
        void merge_before_extend(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size);
        void merge_before_page_allocation(uint64_t requested_pages);
        [[nodiscard]] bool can_allocate(uint64_t pages) const;
        uint64_t allocate(uint64_t pages);
        uint64_t release(uint64_t pages);
    };

    struct ScopedState {
        HiCacheTokenRadixTree tree;
        HiCacheStorageDirectory storage;
        HiCacheAsyncOperationTable async_ops;
        HiCacheCapacityIndex capacity;
        HiCacheRefLedger refs;
        HiCacheTargetControlClock clock;
        DeviceAllocatorLedger device_allocator;
        std::unordered_map<std::string, RequestState> requests;
        std::vector<PendingWriteThroughBackup> pending_write_through_backups;
    };

    /**
     * @brief 一次 host allocator 申请在 target capacity 下的结果。
     *
     * 当前仍使用 capacity/reservation 投影，不引入独立 host allocator 对象；所有
     * write backup 和 prefetch 入口都必须经由该结构收敛申请、清理、截断语义。
     */
    struct HostAllocationResult {
        uint64_t requested_pages = 0;
        uint64_t accepted_pages = 0;
        uint64_t capacity_pages = 0;
        uint64_t occupied_pages = 0;
        uint64_t reserved_pages = 0;
        bool accepted = false;
        bool truncated = false;
    };

    /**
     * @brief storage prefetch I/O progress 的独立估计结果。
     *
     * 该层只回答“已经传到 host 的 prefix 是多少”，不决定 policy 是否可以
     * terminate。当前实现是 zero-progress placeholder，后续可替换成校准模型。
     */
    struct PrefetchIoProgressEstimate {
        std::vector<std::string> completed_pages;
        std::string model_name;
        std::string reason;
    };

    /**
     * @brief 单次 prefetch checkpoint/request 边界的 target progress 估计。
     *
     * storage hit 只说明 L3 中存在连续 prefix；completed pages 才表示 terminate
     * 时已经传输到 host，可安全插入 host radix。
     */
    struct PrefetchProgressEstimate {
        std::vector<std::string> completed_pages;
        uint64_t storage_hit_pages = 0;
        bool storage_hit_sufficient = false;
        bool fully_completed = false;
        bool terminal_checkpoint = false;
        bool timeout_elapsed = false;
        std::string reason;
    };

    HiCacheConfig config_;
    HiCacheTargetPager pager_;
    HiCacheTokenDirectory token_directory_;
    HiCachePolicy policy_;
    std::unordered_map<std::string, ScopedState> scopes_;
    uint64_t policy_decision_epoch_ = 0;
    std::vector<HiCachePolicyDecisionRecord> policy_decisions_;

    [[nodiscard]] std::string normalized_scope(const HiCacheFact & fact) const;
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;
    [[nodiscard]] ScopedState & scope_state(const HiCacheFact & fact);
    void record_token_resolution(const HiCacheFact & fact, HiCacheSummary & summary, const HiCacheTokenResolution & resolution) const;
    [[nodiscard]] HiCachePagePath page_path_from_resolution(const HiCacheFact & fact, const HiCacheTokenResolution & resolution) const;
    void ensure_device_allocator(ScopedState & scope);
    /** @brief 判断新 materialize 的 device page 是否会在当前 insert 边界留下可观测 dirty 生命周期。 */
    [[nodiscard]] bool inserted_device_dirty_visible_at_insert_boundary() const;
    /** @brief 估计 storage prefetch I/O 在当前 target 边界已经完成的 page prefix。 */
    [[nodiscard]] PrefetchIoProgressEstimate estimate_prefetch_io_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact) const;
    /** @brief 估计 SGLang terminate_prefetch 边界可见的 completed prefix。 */
    [[nodiscard]] PrefetchProgressEstimate estimate_prefetch_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact,
                                                                      bool require_full_completion) const;
    void drain_write_through_backup_refs(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         ScopedState & scope, const std::string & reason);

    void sync_capacity(ScopedState & scope, const std::string & cache_scope, const std::vector<HiCacheNodeId> & node_ids, const std::string & reason);
    void sync_capacity_for_insert(ScopedState & scope, const std::string & cache_scope, const HiCacheInsertResult & insert, const std::string & reason);
    void sync_capacity_for_ref(ScopedState & scope, const std::string & cache_scope, const HiCacheRefMutation & mutation, const std::string & reason);
    void record_policy_decision(const HiCacheFact & fact, HiCachePolicyDecisionRecord decision);

    void apply_request_bound_match_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_admission(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_lifecycle_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_storage_backend_readable(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    /** @brief 将已完成的 target prefetch prefix 统一落到 host radix / storage directory。 */
    void apply_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                              HiCachePrefetchOperation & op);
    /** @brief 取消 prefetch operation，并保留尚未 drain 的 host reservation。 */
    void cancel_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         ScopedState & scope, HiCachePrefetchOperation & op, const std::string & transition_kind,
                                         HiCachePrefetchState prefetch_state);
    /** @brief 在 request 重新进入调度主链路前，补齐 SGLang check_prefetch_progress 的完成边界。 */
    void resolve_prefetch_before_request_use(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                             ScopedState & scope);

    void update_request_state(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages);
    [[nodiscard]] HiCacheInsertResult insert_request_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                          ScopedState & scope, const std::vector<std::string> & pages);
    void apply_write_count_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                                  const std::vector<std::string> & pages);
    void enforce_device_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                                 uint64_t requested_pages);
    void enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                               uint64_t requested_pages);
    void drain_deferred_host_releases(const HiCacheFact & fact, ScopedState & scope);
    [[nodiscard]] HostAllocationResult request_host_allocation(const HiCacheFact & fact, HiCacheSummary & summary,
                                                               std::vector<HiCacheStateTransition> & transitions, ScopedState & scope, uint64_t requested_pages,
                                                               uint64_t minimum_pages, bool allow_truncate, const std::string & reason);
    [[nodiscard]] uint64_t evict_device_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                             ScopedState & scope, HiCacheNodeId node_id);
    [[nodiscard]] uint64_t evict_host_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                           ScopedState & scope, HiCacheNodeId node_id);
    [[nodiscard]] bool commit_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                          ScopedState & scope, HiCacheNodeId node_id, bool storage_readable);
    void hold_write_through_backup_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                       ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages);
    void record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & kind,
                           const std::string & tier, const std::vector<std::string> & pages, const std::string & before_digest);
};

/** @brief 运行 HiCache state model 并返回 summary；不修改 DAG。 */
[[nodiscard]] HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config);

} // namespace TraceGraph
