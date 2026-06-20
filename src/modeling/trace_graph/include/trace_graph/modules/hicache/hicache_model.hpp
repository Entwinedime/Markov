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
        std::vector<std::string> full_pages;
        std::vector<std::string> device_pages;
        std::vector<std::string> host_pages;
        std::vector<HiCacheNodeId> device_chain;
        std::vector<HiCacheNodeId> host_chain;
        uint64_t device_reservation_pages = 0;
        std::string lifecycle_state;
    };

    struct ScopedState {
        HiCacheTokenRadixTree tree;
        HiCacheStorageDirectory storage;
        HiCacheAsyncOperationTable async_ops;
        HiCacheCapacityIndex capacity;
        HiCacheRefLedger refs;
        HiCacheTargetControlClock clock;
        std::unordered_map<std::string, RequestState> requests;
    };

    HiCacheConfig config_;
    HiCacheTargetPager pager_;
    HiCacheTokenPathStore token_store_;
    HiCachePolicy policy_;
    std::unordered_map<std::string, ScopedState> scopes_;
    uint64_t policy_decision_epoch_ = 0;
    std::vector<HiCachePolicyDecisionRecord> policy_decisions_;

    [[nodiscard]] std::string normalized_scope(const HiCacheFact & fact) const;
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;
    [[nodiscard]] ScopedState & scope_state(const HiCacheFact & fact);
    [[nodiscard]] HiCacheTokenPath tokens_for_fact(const HiCacheFact & fact, HiCacheSummary & summary) const;
    [[nodiscard]] HiCachePagePath page_path_for_fact(const HiCacheFact & fact, HiCacheSummary & summary) const;

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

    void update_request_state(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages);
    void insert_request_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                             const std::vector<std::string> & pages);
    void apply_write_count_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                                  const std::vector<std::string> & pages);
    void enforce_device_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                                 uint64_t requested_pages);
    void enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                               uint64_t requested_pages);
    void evict_device_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                           HiCacheNodeId node_id);
    void evict_host_node(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                         HiCacheNodeId node_id);
    void commit_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, ScopedState & scope,
                            HiCacheNodeId node_id, bool storage_readable);
    void record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & kind,
                           const std::string & tier, const std::vector<std::string> & pages, const std::string & before_digest);
};

/** @brief 运行 HiCache state model 并返回 summary；不修改 DAG。 */
[[nodiscard]] HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config);

} // namespace TraceGraph
