#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_fact.hpp"
#include "trace_graph/modules/hicache/hicache_router.hpp"
#include "trace_graph/modules/hicache/hicache_state_index.hpp"
#include "trace_graph/modules/hicache/hicache_summary.hpp"
#include "trace_graph/modules/hicache/hicache_target_pager.hpp"
#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"
#include "trace_graph/modules/hicache/hicache_token_store.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

class DagGraph;

/**
 * @brief HiCache target state 的确定性投影器。
 *
 * 这个模型有意比完整 SGLang runtime replay 更窄：
 *
 *  - 只消费 hicache_router.{hpp,cpp} 放行的普通 atomic invariant fact。
 *  - target state 由 target config、target page projection、建模出来的 radix
 *    topology 以及 target-side resource policy 推导。
 *  - 不把 source_actual、timing_observation、oracle_state、debug 字段或
 *    diagnostic-injection fact 当作模型输入。
 *  - 只输出状态摘要和 transition trace；DAG mutation 仍然属于后续层。
 *
 * 主状态拆成三块：
 *
 *  - DeviceCacheState：target device radix、L1 resident、request lock 和
 *    admission pressure。
 *  - HostCacheState：target host radix、L2/backuped/storage-visible 拓扑、
 *    page-level host ref，以及 ready 但尚未 visible 的 prefetch 生命周期。
 *  - AsyncState：建模 prefetch work 生命周期，并区分 requested host budget 和
 *    实际 reservation。
 *
 * 不要在这个类里恢复 source-derived 兼容路径。新增行为必须来自 target
 * config / modeled state，或者作为新的 target-independent atomic invariant
 * fact 引入。
 */
class HiCacheState {
public:
    /** @brief 基于固定 target 配置构造一个空的 target-state 状态模型。 */
    explicit HiCacheState(HiCacheConfig config = HiCacheConfig{});

    /** @name 最终 tier 与生命周期视图 */
    /** @{ */
    [[nodiscard]] const std::set<std::string> & l1() const { return state_index_.l1(); }
    [[nodiscard]] const std::set<std::string> & l2() const { return state_index_.l2(); }
    [[nodiscard]] const std::set<std::string> & l3() const { return state_index_.l3(); }
    [[nodiscard]] const std::set<std::string> & dirty() const { return state_index_.dirty(); }
    [[nodiscard]] const std::set<std::string> & backuped() const { return state_index_.backuped(); }
    [[nodiscard]] const std::set<std::string> & evicted() const { return state_index_.evicted(); }
    [[nodiscard]] const std::set<std::string> & locked() const { return state_index_.locked(); }
    [[nodiscard]] const std::set<std::string> & prefetch_planned() const { return state_index_.prefetch_planned(); }
    [[nodiscard]] const std::set<std::string> & prefetch_ready() const { return state_index_.prefetch_ready(); }
    [[nodiscard]] const std::set<std::string> & prefetch_late() const { return state_index_.prefetch_late(); }
    [[nodiscard]] const std::set<std::string> & prefetch_suppressed() const { return state_index_.prefetch_suppressed(); }
    [[nodiscard]] std::set<std::string> pending_writeback() const;
    [[nodiscard]] std::map<std::string, uint64_t> hit_counts() const { return state_index_.page_hit_count_summary(); }
    /** @} */

    /** @brief 返回 modeled state 的确定性摘要。 */
    [[nodiscard]] std::string digest() const;

    /**
     * @brief 将一个已经路由完成的 atomic invariant fact 应用到 target state。
     *
     * 调用方负责 role 路由和必需字段校验；这里不重新接纳 evidence-only fact。
     */
    [[nodiscard]] std::vector<HiCacheStateTransition> apply_fact(const HiCacheFact & fact, HiCacheFactRole role, HiCacheSummary & summary);

    /**
     * @brief 在 trace 结束时收束仍然打开的 modeled work。
     *
     * 这里有意保持保守：只处理 target-modeled lifecycle state，例如 suppress
     * 尚未完成的 best-effort prefetch work，绝不导入 source final-state
     * observation。
     */
    [[nodiscard]] std::vector<HiCacheStateTransition> finalize(HiCacheSummary & summary);

private:
    /**
     * @brief request-local 的 target execution 视图。
     *
     * full_pages 是 request token path 的 target page projection。
     * matched_device_prefix_pages 以及两条 ancestor chain 是最近一次 target
     * lookup/admission 决策的快照。chain 有意按 radix node 分组：eviction 和
     * lock/ref 转移必须保留节点分组，而不能把所有 page 随意拍平成集合。
     */
    struct RequestExecutionState {
        std::vector<std::string> full_pages;
        std::vector<std::string> matched_device_prefix_pages;
        std::vector<std::vector<std::string>> last_device_chain_groups;
        std::vector<std::vector<std::string>> last_host_chain_groups;
        uint64_t device_reservation_pages = 0;
        std::string lifecycle_state;
    };

    /**
     * @brief target-side request lookup 的结果。
     *
     * device_pages 和 host_visible_pages 必须分开，因为 L1、L2、backuped
     * storage 以及 storage-known page 的 mutation 规则不同。visible_pages 只是
     * target request 当前可见的连续前缀，不代表可以把 source lookup 结果复制进
     * target state。
     */
    struct RequestLookupMatch {
        std::vector<std::string> device_pages;
        std::vector<std::string> host_visible_pages;
        std::vector<std::string> visible_pages;
        std::vector<std::vector<std::string>> device_chain_groups;
        std::vector<std::vector<std::string>> host_chain_groups;
    };

    /**
     * @brief device-side modeled state。
     *
     * radix_trees 按 cache_scope 隔离。request_lock_pages 和 lock_count_by_page
     * 表示 target capacity eviction 需要保护的 request-held device pages；
     * 它们不是 source 侧旧锁增量事件的 replay。
     */
    struct DeviceCacheState {
        std::unordered_map<std::string, HiCacheTokenRadixTree> radix_trees;
        std::unordered_map<std::string, std::set<std::string>> request_lock_pages;
        std::unordered_map<std::string, uint64_t> lock_count_by_page;
    };

    /**
     * @brief host-side modeled state。
     *
     * host_visible_pages 是允许进入 L2/backuped final state 的集合。
     * storage_known_pages 记录已经成为合法 storage-backed candidate 的 page。
     * ready_not_visible_pages 是生命周期账本：page 可以先进入 ready，但只有
     * target checkpoint policy 应用后才会变成 visible。
     */
    struct HostCacheState {
        std::unordered_map<std::string, HiCacheTokenRadixTree> radix_trees;
        std::unordered_map<std::string, std::set<std::string>> request_lock_pages;
        std::unordered_map<std::string, uint64_t> ref_count_by_page;
        std::set<std::string> storage_known_pages;
        std::set<std::string> ready_not_visible_pages;
        std::set<std::string> host_visible_pages;
    };

    /** @brief 单个 async prefetch work item 的 modeled lifecycle。 */
    enum class PrefetchWorkState { Pending, Ready, Applied, Suppressed, Late };

    /**
     * @brief storage prefetch work 的建模状态。
     *
     * planned_pages 对应 SGLang page-aligned prefetch_key；pages 对应 controller
     * storage hit query 后仍保留的连续命中前缀。requested_host_pages 用于
     * prefetch_tokens_occupied / rate-limit，reserved_host_pages 是经过
     * SGLang-style host allocation fallback 后实际能放进 modeled host memory 的数量。
     * 两个 budget 不可合并，否则 cleanup 和 rate limiting 会退化成依赖 observed
     * final L2 counts，而不是 target policy。
     */
    struct PrefetchWorkItem {
        std::string request_key;
        std::string anchor_ref_key;
        std::string cache_scope;
        std::vector<std::string> anchor_pages;
        std::vector<std::string> planned_pages;
        std::vector<std::string> pages;
        uint64_t enqueue_epoch = 0;
        uint64_t checkpoint_epoch = 0;
        uint64_t enqueue_ts = 0;
        uint64_t last_checkpoint_ts = 0;
        int64_t priority = 0;
        bool ignore_eos = false;
        uint64_t requested_host_pages = 0;
        uint64_t reserved_host_pages = 0;
        PrefetchWorkState state = PrefetchWorkState::Pending;
    };

    /**
     * @brief async scheduler 的建模状态。
     *
     * epoch 是 target-derived ordering anchor，不是 source trace 里的 wall-clock
     * completion signal。map 使用 normalized cache_scope:request_id 作为 key，
     * 避免不同 scope 的并发 request 共享 work state。
     */
    struct AsyncState {
        uint64_t scheduler_epoch = 0;
        uint64_t checkpoint_index = 0;
        uint64_t issued_work_units = 0;
        uint64_t completed_work_units = 0;
        std::unordered_map<std::string, PrefetchWorkItem> prefetch_by_request;
    };

    HiCacheConfig config_;
    HiCacheTargetPager target_pager_;
    HiCacheTokenPathStore token_store_;
    HiCacheStateIndex state_index_;
    DeviceCacheState device_state_;
    HostCacheState host_state_;
    std::unordered_map<std::string, RequestExecutionState> request_states_;
    AsyncState async_state_;
    std::unordered_map<std::string, std::set<std::string>> pending_writeback_pages_by_scope_;

    /**
     * @name token 与 page projection
     *
     * 将 invariant token fact 转换成 target page id。projection 是
     * target-derived 的：优先使用配置的 page size，仅在配置缺失时把
     * source_page_size 当作 fact 声明的 invariant page size。
     */
    /** @{ */
    [[nodiscard]] uint64_t page_size_for_fact(const HiCacheFact & fact) const;
    [[nodiscard]] std::vector<std::string> pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;
    [[nodiscard]] uint64_t full_path_token_count(const HiCacheFact & fact) const;
    [[nodiscard]] uint64_t projected_aligned_token_count(const HiCacheFact & fact) const;
    [[nodiscard]] bool needs_projected_pages(const HiCacheFact & fact) const;
    [[nodiscard]] HiCacheTokenPath projected_full_path_tokens_for_fact(const HiCacheFact & fact) const;
    void note_missing_projection_if_needed(const HiCacheFact & fact, HiCacheSummary & summary) const;
    /** @} */

    /**
     * @name scope 与 topology helpers
     *
     * cache_scope 隔离和 radix topology 重建集中在这一组 helper 中。device 与
     * host topology 分开建模，因为 source L1 match、host visibility 和
     * storage-known page 不是等价概念。
     */
    /** @{ */
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;
    [[nodiscard]] std::string normalized_scope(const HiCacheFact & fact) const;
    [[nodiscard]] bool page_in_scope(const std::string & page, const std::string & scope) const;
    [[nodiscard]] uint64_t tier_size_for_scope(const std::string & tier, const std::string & scope) const;
    HiCacheTokenRadixTree & device_radix_tree_for_fact(const HiCacheFact & fact);
    HiCacheTokenRadixTree & host_radix_tree_for_fact(const HiCacheFact & fact);
    [[nodiscard]] RequestLookupMatch match_request_lookup_path(const HiCacheFact & fact, const std::vector<std::string> & full_pages);
    [[nodiscard]] std::vector<std::string> contiguous_resident_prefix(const std::vector<std::string> & full_pages, size_t max_pages, bool include_device,
                                                                      bool include_host, bool include_storage) const;
    void insert_host_path_topology(const HiCacheFact & fact, const std::vector<std::string> & pages);
    void insert_host_page_topology_from_device(const HiCacheFact & fact, const std::string & page);
    /** @} */

    /**
     * @name target policy 参数
     *
     * 影响 residency 和 cleanup 的 knob 统一从这里读取。这里的默认值是 target
     * policy default，不是为了贴合 source observation 的兼容 shim。
     */
    /** @{ */
    [[nodiscard]] std::set<std::string> * tier_set(const std::string & tier);
    [[nodiscard]] const std::set<std::string> * tier_set(const std::string & tier) const;
    [[nodiscard]] std::vector<std::string> * touch_order_for_tier(const std::string & tier);
    [[nodiscard]] uint64_t capacity_for_tier(const std::string & tier) const;
    [[nodiscard]] uint64_t target_write_through_threshold() const;
    [[nodiscard]] bool target_write_count_enabled() const;
    [[nodiscard]] bool target_write_back_enabled() const;
    [[nodiscard]] uint64_t target_prefetch_threshold_pages() const;
    [[nodiscard]] uint64_t target_prefetch_capacity_limit_pages() const;
    [[nodiscard]] uint64_t active_prefetch_requested_pages_for_scope(const std::string & scope) const;
    [[nodiscard]] uint64_t reserved_prefetch_pages_for_scope(const std::string & scope) const;
    [[nodiscard]] uint64_t host_pool_occupied_pages_for_scope(const std::string & scope) const;
    [[nodiscard]] uint64_t host_pool_available_pages_for_scope(const std::string & scope) const;
    /** @} */

    /**
     * @name fact role handlers
     *
     * 每个 handler 对应一个已经放行的 atomic invariant role。Unknown、aggregate、
     * source_actual、timing_observation、oracle 和 debug fact 都在进入这一层前被
     * 排除。
     */
    /** @{ */
    void apply_request_tokens(const HiCacheFact & fact);
    void apply_request_context(const HiCacheFact & fact, HiCacheSummary & summary);
    void apply_request_admission(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_lifecycle_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_bound_match_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_lifecycle_insert(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    /** @} */

    /**
     * @name write policy semantics
     *
     * 三种 write policy 共享 device insert、hit-count backup、device eviction 前处理
     * 和同步 host backup commit。这里不建模 ack 异步时序，只建模策略语义结果。
     */
    /** @{ */
    void apply_write_policy_hit_counts(const HiCacheFact & fact, const std::vector<std::string> & full_pages, HiCacheSummary & summary,
                                       std::vector<HiCacheStateTransition> & transitions);
    void apply_new_device_page_write_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page);
    void apply_device_eviction_write_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page);
    void commit_host_backup_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page, bool storage_resident);
    /** @} */

    /**
     * @name request resource lifetime
     *
     * 跟踪 modeled request lock/ref 和 admission reservation。这些 helper 用来
     * 保护 target eviction 决策，但不会 replay 旧的锁增量事件。
     */
    /** @{ */
    [[nodiscard]] std::vector<std::string> flatten_page_groups(const std::vector<std::vector<std::string>> & groups) const;
    void update_request_path_state(const HiCacheFact & fact, const std::vector<std::string> & full_pages, const std::vector<std::string> & matched_pages,
                                   const std::vector<std::vector<std::string>> & device_chain_groups,
                                   const std::vector<std::vector<std::string>> & host_chain_groups);
    [[nodiscard]] uint64_t active_device_reservation_pages_for_scope(const std::string & scope) const;
    [[nodiscard]] uint64_t estimate_admission_requested_tokens(const HiCacheFact & fact, const RequestExecutionState & request_state) const;
    [[nodiscard]] uint64_t estimate_admission_requested_pages(const HiCacheFact & fact, const RequestExecutionState & request_state) const;
    void clear_device_reservation(const std::string & request_key);
    void acquire_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                     const std::string & request_key, const std::vector<std::string> & pages);
    void release_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                     const std::string & request_key);
    void replace_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                     const std::string & request_key, const std::vector<std::string> & pages);
    void acquire_host_request_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & request_key, const std::vector<std::string> & pages);
    void release_host_request_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & request_key);
    void replace_host_request_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & request_key, const std::vector<std::string> & pages);
    void apply_target_device_pressure(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      uint64_t requested_pages);
    /** @} */

    /**
     * @name async prefetch lifecycle
     *
     * 建模 prefetch admission、readiness、host visibility 和 terminal cleanup。
     * host budget accounting 对齐 SGLang HiRadixCache 行为：按 request size 做
     * cleanup，与 cleanup 后实际 fit 的 reservation 不是同一个量。
     */
    /** @{ */
    [[nodiscard]] uint64_t reserve_host_pages_for_prefetch(const HiCacheFact & fact, HiCacheSummary & summary,
                                                           std::vector<HiCacheStateTransition> & transitions, const std::string & scope,
                                                           uint64_t requested_pages);
    void release_prefetch_reservation(PrefetchWorkItem & work);
    void release_prefetch_work_resources(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         PrefetchWorkItem & work);
    [[nodiscard]] bool prefetch_work_open(const PrefetchWorkItem & work) const;
    [[nodiscard]] bool best_effort_ready_budget_available(const PrefetchWorkItem & work) const;
    [[nodiscard]] bool prefetch_timeout_elapsed(const HiCacheFact & fact, const PrefetchWorkItem & work) const;
    [[nodiscard]] std::vector<std::string> target_prefetch_storage_pages(const std::vector<std::string> & matched_pages, size_t first_page_index) const;
    [[nodiscard]] bool terminal_prefetch_checkpoint(const HiCacheFact & fact) const;
    void complete_prefetch_ready_checkpoint(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            PrefetchWorkItem & work);
    void apply_host_visibility_for_ready_work(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                              PrefetchWorkItem & work);
    void suppress_prefetch_work(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, PrefetchWorkItem & work);
    void mark_prefetch_work_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 PrefetchWorkItem & work);
    /** @} */

    /**
     * @name state mutation primitives
     *
     * 所有集合 mutation 都经过这些 helper，保证 transition emission、
     * dirty/backuped 联动、prefetch mark 和 LRU touch order 保持一致。
     */
    /** @{ */
    void add_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                      const std::string & page);
    void add_host_visible_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void remove_host_visible_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page);
    void remove_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                         const std::string & page);
    void mark_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void clear_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void clear_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void clear_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void clear_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_prefetch_planned(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_prefetch_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_prefetch_suppressed(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page);
    /** @} */

    /**
     * @name capacity、cleanup 与 transition output
     *
     * capacity enforcement 从 modeled topology 和 protection state 中选择 target
     * victim，并在每个有效 mutation 后输出可审计的 transition row。
     */
    /** @{ */
    void touch_page(const std::string & tier, const std::string & page);
    void evict_lru_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                         uint64_t page_count);
    void enforce_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier);
    void enqueue_writeback_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page);
    void complete_writeback_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page);
    uint64_t evict_host_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, uint64_t page_count);
    uint64_t evict_host_pages_for_scope(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                        const std::string & scope, uint64_t page_count);
    void enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    [[nodiscard]] std::string transition_state_digest() const;
    void record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & kind,
                           const std::string & tier, const std::string & page, const std::string & before_digest);
    /** @} */
};

/**
 * @brief 在 DAG trace 上运行 HiCache 状态模型。
 *
 * runner 负责外层 scan/sort/route/validate 循环，实际 target-state mutation 全部
 * 委托给 HiCacheState。
 */
class HiCacheStateModel {
public:
    explicit HiCacheStateModel(HiCacheConfig config);
    [[nodiscard]] HiCacheSummary run(DagGraph & graph);

private:
    HiCacheConfig config_;
    HiCacheFactParser fact_parser_;
    HiCacheState state_;
};

/** @brief module 包装层使用的便捷入口。 */
[[nodiscard]] HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config);

} // namespace TraceGraph
