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
        uint64_t device_reservation_pages = 0;
        std::string lifecycle_state;
    };

    HiCacheConfig config_;
    HiCacheTargetPager target_pager_;
    HiCacheTokenPathStore token_store_;
    std::unordered_map<std::string, HiCacheTokenRadixTree> radix_trees_;
    HiCacheStateIndex state_index_;
    std::unordered_map<std::string, RequestExecutionState> request_states_;
    std::unordered_map<std::string, std::set<std::string>> request_device_lock_pages_;
    std::unordered_map<std::string, uint64_t> device_lock_count_by_page_;
    std::unordered_map<std::string, std::vector<std::string>> pending_prefetch_pages_;
    std::unordered_map<std::string, uint64_t> prefetch_decision_ts_;
    std::unordered_map<std::string, uint64_t> active_prefetch_host_pages_by_request_;
    std::set<std::string> pending_prefetch_host_pressure_requests_;
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
    HiCacheTokenRadixTree & radix_tree_for_fact(const HiCacheFact & fact);
    std::set<std::string> * tier_set(const std::string & tier);
    const std::set<std::string> * tier_set(const std::string & tier) const;
    std::vector<std::string> * touch_order_for_tier(const std::string & tier);
    uint64_t capacity_for_tier(const std::string & tier) const;
    uint64_t target_write_through_threshold() const;
    bool target_write_count_enabled() const;

    void apply_request_tokens(const HiCacheFact & fact);
    void apply_request_context(const HiCacheFact & fact, HiCacheSummary & summary);
    void apply_request_admission(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_lifecycle_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_bound_match_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_request_lifecycle_insert(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_diagnostic_state_injection(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_write_policy_hit_counts(const HiCacheFact & fact, const std::vector<std::string> & full_pages, HiCacheSummary & summary,
                                       std::vector<HiCacheStateTransition> & transitions);
    std::vector<std::string> flatten_page_groups(const std::vector<std::vector<std::string>> & groups) const;
    void update_request_path_state(const HiCacheFact & fact, const std::vector<std::string> & full_pages,
                                   const std::vector<std::string> & matched_pages,
                                   const std::vector<std::vector<std::string>> & chain_groups);
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
    void apply_target_device_pressure(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      uint64_t requested_pages);
    uint64_t active_prefetch_host_pages_for_scope(const std::string & scope) const;
    void apply_pending_prefetch_host_pressure_for_request(const HiCacheFact & fact, HiCacheSummary & summary,
                                                          std::vector<HiCacheStateTransition> & transitions, const std::string & request_key);
    void release_prefetch_host_buffer(const HiCacheFact & fact);

    void add_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
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
    void apply_prefetch_host_pressure(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      uint64_t requested_pages);
    void evict_host_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, uint64_t page_count);
    std::string transition_state_digest() const;
    void record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & kind,
                           const std::string & tier, const std::string & page, const std::string & before_digest);
    std::string page_for_scope(const HiCacheFact & fact, const std::string & page) const;
    std::set<std::string> diagnostic_desired_pages(const HiCacheFact & fact, const std::string & state_key) const;
    void replace_resident_state_for_scope(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                          const std::string & state_key, const std::string & tier);
    void replace_metadata_state_for_scope(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                          const std::string & state_key);
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
