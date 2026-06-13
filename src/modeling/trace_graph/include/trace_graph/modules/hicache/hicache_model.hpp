#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_fact.hpp"
#include "trace_graph/modules/hicache/hicache_router.hpp"
#include "trace_graph/modules/hicache/hicache_state_index.hpp"
#include "trace_graph/modules/hicache/hicache_summary.hpp"
#include "trace_graph/modules/hicache/hicache_target_pager.hpp"
#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"
#include "trace_graph/modules/hicache/hicache_token_store.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

class DagGraph;

class HiCacheState {
  public:
    explicit HiCacheState(HiCacheConfig config = HiCacheConfig{});

    const std::set<std::string> & l1() const { return state_index_.l1(); }
    const std::set<std::string> & l2() const { return state_index_.l2(); }
    const std::set<std::string> & l3() const { return state_index_.l3(); }
    const std::set<std::string> & dirty() const { return state_index_.dirty(); }
    const std::set<std::string> & backuped() const { return state_index_.backuped(); }
    const std::set<std::string> & evicted() const { return state_index_.evicted(); }
    const std::set<std::string> & locked() const { return state_index_.locked(); }
    const std::set<std::string> & prefetch_planned() const { return state_index_.prefetch_planned(); }
    const std::set<std::string> & prefetch_ready() const { return state_index_.prefetch_ready(); }
    const std::set<std::string> & prefetch_late() const { return state_index_.prefetch_late(); }
    const std::set<std::string> & prefetch_suppressed() const { return state_index_.prefetch_suppressed(); }
    std::set<std::string> pending_writeback() const;
    std::map<std::string, uint64_t> hit_counts() const { return state_index_.page_hit_count_summary(); }

    std::string digest() const;
    std::vector<HiCacheStateTransition> apply_fact(const HiCacheFact & fact, HiCacheFactRole role, HiCacheSummary & summary);
    std::vector<HiCacheStateTransition> finalize(HiCacheSummary & summary);

  private:
    struct RequestExecutionState {
        std::vector<std::string> full_pages;
        std::vector<std::string> matched_device_prefix_pages;
        std::vector<std::vector<std::string>> last_device_chain_groups;
        std::vector<std::vector<std::string>> last_host_chain_groups;
        uint64_t device_reservation_pages = 0;
        std::string lifecycle_state;
    };

    struct RequestLookupMatch {
        std::vector<std::string> device_pages;
        std::vector<std::string> host_visible_pages;
        std::vector<std::string> visible_pages;
        std::vector<std::vector<std::string>> device_chain_groups;
        std::vector<std::vector<std::string>> host_chain_groups;
    };

    struct DeviceCacheState {
        std::unordered_map<std::string, HiCacheTokenRadixTree> radix_trees;
        std::unordered_map<std::string, std::set<std::string>> request_lock_pages;
        std::unordered_map<std::string, uint64_t> lock_count_by_page;
    };

    struct HostCacheState {
        std::unordered_map<std::string, HiCacheTokenRadixTree> radix_trees;
        std::unordered_map<std::string, std::set<std::string>> request_lock_pages;
        std::unordered_map<std::string, uint64_t> ref_count_by_page;
        std::set<std::string> storage_known_pages;
        std::set<std::string> ready_not_visible_pages;
        std::set<std::string> host_visible_pages;
    };

    enum class PrefetchWorkState { Pending, Ready, Applied, Suppressed, Late };

    struct PrefetchWorkItem {
        std::string request_key;
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

    uint64_t page_size_for_fact(const HiCacheFact & fact) const;
    std::vector<std::string> pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;
    uint64_t full_path_token_count(const HiCacheFact & fact) const;
    uint64_t projected_aligned_token_count(const HiCacheFact & fact) const;
    bool needs_projected_pages(const HiCacheFact & fact) const;
    HiCacheTokenPath projected_full_path_tokens_for_fact(const HiCacheFact & fact) const;
    void note_missing_projection_if_needed(const HiCacheFact & fact, HiCacheSummary & summary) const;
    std::string scoped_request_key(const HiCacheFact & fact) const;
    std::string normalized_scope(const HiCacheFact & fact) const;
    bool page_in_scope(const std::string & page, const std::string & scope) const;
    uint64_t tier_size_for_scope(const std::string & tier, const std::string & scope) const;
    HiCacheTokenRadixTree & device_radix_tree_for_fact(const HiCacheFact & fact);
    HiCacheTokenRadixTree & host_radix_tree_for_fact(const HiCacheFact & fact);
    RequestLookupMatch match_request_lookup_path(const HiCacheFact & fact, const std::vector<std::string> & full_pages);
    std::vector<std::string> contiguous_resident_prefix(const std::vector<std::string> & full_pages, size_t max_pages, bool include_device, bool include_host,
                                                        bool include_storage) const;
    void insert_host_path_topology(const HiCacheFact & fact, const std::vector<std::string> & pages);
    void insert_host_page_topology_from_device(const HiCacheFact & fact, const std::string & page);
    std::set<std::string> * tier_set(const std::string & tier);
    const std::set<std::string> * tier_set(const std::string & tier) const;
    std::vector<std::string> * touch_order_for_tier(const std::string & tier);
    uint64_t capacity_for_tier(const std::string & tier) const;
    uint64_t target_write_through_threshold() const;
    bool target_write_count_enabled() const;
    uint64_t target_prefetch_threshold_pages() const;
    uint64_t target_prefetch_capacity_limit_pages() const;
    uint64_t active_prefetch_requested_pages_for_scope(const std::string & scope) const;
    uint64_t reserved_prefetch_pages_for_scope(const std::string & scope) const;
    uint64_t host_pool_occupied_pages_for_scope(const std::string & scope) const;
    uint64_t host_pool_available_pages_for_scope(const std::string & scope) const;

    void apply_request_tokens(const HiCacheFact & fact);
    void apply_request_context(const HiCacheFact & fact, HiCacheSummary & summary);
    void apply_request_admission(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_lifecycle_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_bound_match_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_lifecycle_insert(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_write_policy_hit_counts(const HiCacheFact & fact, const std::vector<std::string> & full_pages, HiCacheSummary & summary,
                                       std::vector<HiCacheStateTransition> & transitions);
    std::vector<std::string> flatten_page_groups(const std::vector<std::vector<std::string>> & groups) const;
    void update_request_path_state(const HiCacheFact & fact, const std::vector<std::string> & full_pages, const std::vector<std::string> & matched_pages,
                                   const std::vector<std::vector<std::string>> & device_chain_groups,
                                   const std::vector<std::vector<std::string>> & host_chain_groups);
    uint64_t active_device_reservation_pages_for_scope(const std::string & scope) const;
    uint64_t estimate_admission_requested_tokens(const HiCacheFact & fact, const RequestExecutionState & request_state) const;
    uint64_t estimate_admission_requested_pages(const HiCacheFact & fact, const RequestExecutionState & request_state) const;
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
    uint64_t reserve_host_pages_for_prefetch(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                             const std::string & scope, uint64_t requested_pages);
    void release_prefetch_reservation(PrefetchWorkItem & work);
    bool best_effort_ready_budget_available(const PrefetchWorkItem & work) const;
    std::vector<std::string> target_prefetch_storage_pages(const std::vector<std::string> & matched_pages) const;
    bool terminal_prefetch_checkpoint(const HiCacheFact & fact) const;
    void complete_prefetch_ready_checkpoint(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            PrefetchWorkItem & work);
    void apply_host_visibility_for_ready_work(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                              PrefetchWorkItem & work);
    void suppress_prefetch_work(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, PrefetchWorkItem & work);
    void mark_prefetch_work_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 PrefetchWorkItem & work);

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
    void touch_page(const std::string & tier, const std::string & page);
    void evict_lru_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                         uint64_t page_count);
    void enforce_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier);
    void flush_dirty_page_to_host(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page);
    void enqueue_writeback_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page);
    void complete_writeback_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page);
    uint64_t evict_host_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, uint64_t page_count);
    uint64_t evict_host_pages_for_scope(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                        const std::string & scope, uint64_t page_count);
    void enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    std::string transition_state_digest() const;
    void record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & kind,
                           const std::string & tier, const std::string & page, const std::string & before_digest);
};

class HiCacheStateModel {
  public:
    explicit HiCacheStateModel(HiCacheConfig config);
    HiCacheSummary run(DagGraph & graph);

  private:
    HiCacheConfig config_;
    HiCacheFactParser fact_parser_;
    HiCacheState state_;
};

HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config);

} // namespace TraceGraph
