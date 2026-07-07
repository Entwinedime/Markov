/**
 * @file
 * @brief HiCache canonical-radix target-derived 状态机入口。
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"
#include "markov/trace_graph/modules/hicache/model/summary.hpp"
#include "markov/trace_graph/modules/hicache/policy.hpp"
#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"
#include "markov/trace_graph/modules/hicache/router.hpp"
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"
#include "markov/trace_graph/modules/hicache/runtime/capacity_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/ref_ledger.hpp"
#include "markov/trace_graph/modules/hicache/runtime/state_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_control_clock.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_pager.hpp"
#include "markov/trace_graph/modules/hicache/runtime/token_store.hpp"
#include "markov/trace_graph/modules/hicache/storage/storage_directory.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

using radix::HiCacheCacheNode;
using radix::HiCacheHostEvictionResult;
using radix::HiCacheInsertResult;
using radix::HiCacheNodeId;
using radix::HiCacheNodeSplitRecord;
using radix::HiCacheTokenRadixTree;
using runtime::hicache_derived_state_mode_name;
using runtime::hicache_sorted_vector;
using runtime::hicache_token_resolution_status_name;
using runtime::HiCacheAsyncOperationTable;
using runtime::HiCacheCapacityAuditIssue;
using runtime::HiCacheCapacityIndex;
using runtime::HiCacheCapacityMutation;
using runtime::HiCacheCapacityVictimChoice;
using runtime::HiCacheControlBoundary;
using runtime::HiCacheDerivedStateMode;
using runtime::HiCacheDerivedStateSnapshot;
using runtime::HiCacheDerivedStateView;
using runtime::HiCacheLoadbackOperation;
using runtime::HiCacheOperationHeader;
using runtime::HiCacheOperationKind;
using runtime::HiCacheOperationLifecycleTransition;
using runtime::HiCacheOperationState;
using runtime::HiCachePagePath;
using runtime::HiCachePrefetchOperation;
using runtime::HiCachePrefetchState;
using runtime::HiCacheProjectedPage;
using runtime::HiCacheRefAuditIssue;
using runtime::HiCacheRefLedger;
using runtime::HiCacheRefMutation;
using runtime::HiCacheStorageOperation;
using runtime::HiCacheTargetControlClock;
using runtime::HiCacheTargetPager;
using runtime::HiCacheTokenDirectory;
using runtime::HiCacheTokenResolution;
using runtime::HiCacheTokenResolutionStatus;
using runtime::HiCacheWritebackOperation;
using storage::HiCacheStorageDirectory;

/**
 * @brief HiCache target state 的 canonical-radix 建模器。
 *
 * 每个 cache_scope 维护一棵 canonical radix tree，node residency/ref 是唯一状态源；
 * L1/L2/L3、dirty、backuped、evicted、locked 和 prefetch lifecycle 都从 tree/async
 * table 派生到 summary。
 */
class HiCacheState {
public:
    /** @brief 使用显式 target config 初始化状态机。 */
    explicit HiCacheState(frontend::HiCacheConfig config = frontend::HiCacheConfig{});

    /** @brief 按 fact role 推进 canonical state，Debug 构建才写入 transition buffer。 */
    void apply_fact(const HiCacheFact & fact, HiCacheFactRole role, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief 在 trace 结束时收敛 pending 生命周期，Debug 构建才写入 transition buffer。 */
    void finalize(HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief 从 canonical tree/async/storage 结构派生可验证的 final-state snapshot。 */
    [[nodiscard]] HiCacheDerivedStateSnapshot derived_state(HiCacheDerivedStateMode mode = HiCacheDerivedStateMode::MaterializedOnly) const;

    /** @brief 当前仍持有 lock/host ref 的 owner 数。 */
    [[nodiscard]] uint64_t active_ref_owner_count() const;

    /** @brief 已发生的 radix split 次数。 */
    [[nodiscard]] uint64_t radix_split_count() const;

#ifdef DEBUG
    /** @brief 返回 radix split 的结构化审计 trace。 */
    [[nodiscard]] std::vector<HiCacheNodeSplitRecord> radix_split_trace() const;
#endif

    /** @brief target control clock 已记录的 boundary 数。 */
    [[nodiscard]] uint64_t control_boundary_count() const;

#ifdef DEBUG
    /** @brief 返回 target control boundary 的结构化 trace。 */
    [[nodiscard]] std::vector<HiCacheControlBoundary> control_boundary_trace() const;
#endif

    /** @brief async operation lifecycle transition 数。 */
    [[nodiscard]] uint64_t async_lifecycle_transition_count() const;

#ifdef DEBUG
    /** @brief 返回 async operation lifecycle 的结构化审计 trace。 */
    [[nodiscard]] std::vector<HiCacheOperationLifecycleTransition> async_lifecycle_trace() const;
#endif

    /** @brief target policy 决策记录数。 */
    [[nodiscard]] uint64_t policy_decision_count() const;

#ifdef DEBUG
    /** @brief 返回 target policy 决策 trace；用于解释模型为什么接受、等待、截断或回退。 */
    [[nodiscard]] const std::vector<HiCachePolicyDecisionRecord> & policy_decision_trace() const { return policy_decisions_; }
#endif

    /** @brief target storage directory 中已知 page 数。 */
    [[nodiscard]] uint64_t storage_known_page_count() const;

    /** @brief target storage directory 中对 host/device 可读的 page 数。 */
    [[nodiscard]] uint64_t storage_readable_page_count() const;

    /** @brief target backend hash directory 中可读 page 数。 */
    [[nodiscard]] uint64_t storage_backend_readable_count() const;

    /** @brief 已 materialize 到 host/device tree 的 storage page 数。 */
    [[nodiscard]] uint64_t storage_materialized_page_count() const;

    /** @brief capacity index mutation 数。 */
    [[nodiscard]] uint64_t capacity_mutation_count() const;

    /** @brief capacity victim 选择记录数。 */
    [[nodiscard]] uint64_t capacity_victim_choice_count() const;

#ifdef DEBUG
    /** @brief 返回 capacity index mutation 的审计 trace。 */
    [[nodiscard]] std::vector<HiCacheCapacityMutation> capacity_mutation_trace() const;

    /** @brief 返回 capacity victim 选择过程的解释 trace。 */
    [[nodiscard]] std::vector<HiCacheCapacityVictimChoice> capacity_victim_choices() const;

    /** @brief 返回 capacity index 与 canonical tree 的一致性审计问题。 */
    [[nodiscard]] std::vector<HiCacheCapacityAuditIssue> capacity_audit_issues() const;
#endif

    /** @brief ref ledger mutation 数。 */
    [[nodiscard]] uint64_t ref_mutation_count() const;

#ifdef DEBUG
    /** @brief 返回 ref lifecycle mutation 的审计 trace。 */
    [[nodiscard]] std::vector<HiCacheRefMutation> ref_mutation_trace() const;

    /** @brief 返回 ref ledger 与 canonical tree 的一致性审计问题。 */
    [[nodiscard]] std::vector<HiCacheRefAuditIssue> ref_audit_issues() const;
#endif

    /** @brief 生成当前 canonical state 的稳定摘要，用于自检和日志定位。 */
    [[nodiscard]] std::string digest() const;

private:
    /** @brief request-local lifecycle 状态；只缓存 request 视角，不成为 residency 状态源。 */
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

    /** @brief 等待下一轮 target control 或 finalize 边界 drain 的 write-through backup lock。 */
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

        /** @brief 用 target capacity 初始化 allocator 投影。 */
        void configure(uint64_t pages, bool sort_required);

        /** @brief 返回 SGLang allocator gate 会看到的可用 page 数。 */
        [[nodiscard]] uint64_t available_pages() const;

        /** @brief 判断本次申请前是否需要触发 device eviction。 */
        [[nodiscard]] bool should_evict(uint64_t requested_pages) const;

        /** @brief 将 pending release pages 合并回 free pages。 */
        void merge_release_pages();

        /** @brief 在 extend 前模拟 allocator 对 release queue 的可见同步。 */
        void merge_before_extend(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size);

        /** @brief 在真实 page allocation 前模拟 allocator 可见的 release queue 同步。 */
        void merge_before_page_allocation(uint64_t requested_pages);

        /** @brief 判断当前 free pages 是否足够完成申请。 */
        [[nodiscard]] bool can_allocate(uint64_t pages) const;

        /** @brief 消耗 free pages；返回实际成功申请的 page 数。 */
        uint64_t allocate(uint64_t pages);

        /** @brief 把释放 page 先记入 release queue 投影。 */
        uint64_t release(uint64_t pages);
    };

    /** @brief 单个 cache_scope 下的全部 canonical runtime state。 */
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
        uint64_t prefetch_worker_available_ts = 0;
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
     * @brief 单次 prefetch target boundary 的 progress 估计。
     *
     * storage hit 只说明 L3 中存在连续 prefix；completed pages 才表示 terminate
     * 时已经传输到 host，可安全插入 host radix。
     */
    struct PrefetchProgressEstimate {
        std::vector<std::string> completed_pages;
        uint64_t storage_hit_pages = 0;
        bool storage_hit_sufficient = false;
        bool fully_completed = false;
        bool terminal_boundary = false;
        bool timeout_elapsed = false;
        std::string reason;
    };

    /** @brief batch-level cache extend 中单个 request 的 allocator intent。 */
    struct CacheExtendRequestIntent {
        std::string request_key;
        std::string request_id;
        uint64_t accepted_tokens = 0;
        uint64_t target_device_prefix_tokens = 0;
        uint64_t prior_committed_prefix_tokens = 0;
        uint64_t allocation_prefix_tokens = 0;
        uint64_t extend_tokens = 0;
        uint64_t requested_pages = 0;
        uint64_t allocated_pages = 0;
        std::vector<std::string> full_pages;
        std::vector<std::string> device_pages;
        std::vector<std::string> host_pages;
    };

    /** @brief 一次 `cache_extend_input` fact 对应的 batch allocator intent。 */
    struct CacheExtendBatchIntent {
        uint64_t batch_size = 0;
        uint64_t total_extend_tokens = 0;
        uint64_t requested_pages = 0;
        uint64_t allocated_pages = 0;
        std::vector<CacheExtendRequestIntent> requests;
    };

    frontend::HiCacheConfig config_;
    HiCacheTargetPager pager_;
    HiCacheTokenDirectory token_directory_;
    HiCachePolicy policy_;
    std::unordered_map<std::string, ScopedState> scopes_;
    uint64_t policy_decision_epoch_ = 0;
#ifdef DEBUG
    std::vector<HiCachePolicyDecisionRecord> policy_decisions_;
#endif

    /** @brief 归一化 fact 所属 cache scope，缺失时回退到默认 scope。 */
    [[nodiscard]] std::string normalized_scope(const HiCacheFact & fact) const;

    /** @brief 生成 scope/request 复合 key，避免跨 scope request_id 冲突。 */
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;

    /** @brief 获取或创建 fact 所属 cache_scope 的 canonical runtime state。 */
    [[nodiscard]] ScopedState & scope_state(const HiCacheFact & fact);

    /** @brief 把 token resolution 结果写入 summary，供 input contract 诊断。 */
    void record_token_resolution(const HiCacheFact & fact, HiCacheSummary & summary, const HiCacheTokenResolution & resolution) const;

    /** @brief 将 resolved token path 投影成 target page path。 */
    [[nodiscard]] HiCachePagePath page_path_from_resolution(const HiCacheFact & fact, const HiCacheTokenResolution & resolution) const;

    /** @brief 按 target config 延迟初始化 device allocator 投影。 */
    void ensure_device_allocator(ScopedState & scope);
    /** @brief 判断新 materialize 的 device page 是否会在当前 insert 边界留下可观测 dirty 生命周期。 */
    [[nodiscard]] bool inserted_device_dirty_visible_at_insert_boundary() const;
    /** @brief 估计 storage prefetch I/O 在当前 target 边界已经完成的 page prefix。 */
    [[nodiscard]] PrefetchIoProgressEstimate estimate_prefetch_io_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact) const;
    /** @brief 估计 SGLang terminate_prefetch 边界可见的 completed prefix。 */
    [[nodiscard]] PrefetchProgressEstimate estimate_prefetch_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact,
                                                                      bool terminal_boundary) const;
    /** @brief 判断 revoke 产生的 storage-control release 是否会在当前 extend side effect 前可见。 */
    [[nodiscard]] bool prefetch_release_visible_before_cache_extend(const HiCachePrefetchOperation & op, const HiCacheFact & boundary_fact) const;
    void drain_write_through_backup_refs(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                         const std::string & reason);

    /** @brief 将 tree/ref/reservation mutation 同步到 capacity index。 */
    void sync_capacity(ScopedState & scope, const std::string & cache_scope, const std::vector<HiCacheNodeId> & node_ids, const std::string & reason);

    /** @brief 将 insert 造成的 node 变化同步到 capacity index。 */
    void sync_capacity_for_insert(ScopedState & scope, const std::string & cache_scope, const HiCacheInsertResult & insert, const std::string & reason);

    /** @brief 将 ref 变化影响到的 node 同步到 capacity index。 */
    void sync_capacity_for_ref(ScopedState & scope, const std::string & cache_scope, const HiCacheRefMutation & mutation, const std::string & reason);

    /** @brief 记录一次 target policy 决策，并分配稳定 epoch。 */
    void record_policy_decision(const HiCacheFact & fact, HiCachePolicyDecisionRecord decision);

    /** @brief 当前构建是否需要构造 Debug/validation 行级记录。 */
    [[nodiscard]] static constexpr bool debug_records_enabled() {
#ifdef DEBUG
        return true;
#else
        return false;
#endif
    }

    /** @brief Release 构建不为 transition/debug 证据派生 state digest。 */
    [[nodiscard]] std::string debug_state_digest() const {
        if constexpr (debug_records_enabled()) return digest();
        return {};
    }

    /** @brief 处理 cache lookup 输入，建立可复用 prefix 的 request-local 保护。 */
    void apply_cache_lookup_input(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief 处理 batch-level cache extend 输入，申请 KV page 并更新 request ownership。 */
    void apply_cache_extend_input(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief 处理 request lifecycle commit 边界，释放 request-local 保护。 */
    void apply_cache_lifecycle_commit(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief 处理 prefetch candidate 输入。 */
    void apply_prefetch_candidate_anchor(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief 将已完成的 target prefetch prefix 统一落到 host radix / storage directory。 */
    void apply_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                              HiCachePrefetchOperation & op);
    /** @brief 取消 prefetch operation，并保留尚未 drain 的 host reservation。 */
    void cancel_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                         HiCachePrefetchOperation & op, const std::string & transition_kind, HiCachePrefetchState prefetch_state);
    /** @brief 在 cache extend side effect 前结算同 request 的 active prefetch。 */
    void settle_prefetch_before_cache_extend(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                             const std::string & request_key);
    /** @brief 在明确的 scheduler/extend 边界释放 terminal prefetch 的 pending host reservation。 */
    void drain_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                        const std::string & request_key, const std::string & capacity_reason, const std::string & policy_reason);

    /** @brief 更新 request-local committed path 和保护 page 统计。 */
    void update_request_state(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages);

    /** @brief 将 request path 插入 device radix，并返回 insert 审计结果。 */
    [[nodiscard]] HiCacheInsertResult insert_request_path(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                          ScopedState & scope, const std::vector<std::string> & pages);

    /** @brief 根据 write policy 处理 request path 的 host/storage backup。 */
    void apply_write_count_policy(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                  const std::vector<std::string> & pages);

    /** @brief 在 device allocation 前执行 target-derived device capacity enforcement。 */
    void enforce_device_capacity(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                 uint64_t requested_pages);

    /** @brief 在 host insertion/reservation 前执行 target-derived host capacity enforcement。 */
    void enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                               uint64_t requested_pages);

    /** @brief 申请 host reservation，可按调用方语义截断或拒绝。 */
    [[nodiscard]] HostAllocationResult request_host_allocation(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                               ScopedState & scope, uint64_t requested_pages, uint64_t minimum_pages, bool allow_truncate,
                                                               const std::string & reason);

    /** @brief 驱逐单个 device node，并同步 tree、allocator、capacity index 和 transition。 */
    [[nodiscard]] uint64_t evict_device_node(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                             HiCacheNodeId node_id);

    /** @brief 驱逐单个 host leaf，并同步 tree、capacity index 和 transition。 */
    [[nodiscard]] uint64_t evict_host_node(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                           HiCacheNodeId node_id);
    [[nodiscard]] bool commit_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                          HiCacheNodeId node_id, bool storage_readable);
    void hold_write_through_backup_ref(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                       HiCacheNodeId node_id, const std::vector<std::string> & pages);
    void record_transition(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, const std::string & kind,
                           const std::string & tier, const std::vector<std::string> & pages, const std::string & before_digest);
};

/** @brief 运行 HiCache state model 并返回 summary；不修改 DAG。 */
[[nodiscard]] HiCacheSummary apply_hicache_model(core::DagGraph & graph, const frontend::HiCacheConfig & config);

} // namespace markov::trace_graph::modules::hicache::model
