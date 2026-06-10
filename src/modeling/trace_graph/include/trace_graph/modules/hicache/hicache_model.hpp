#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_fact.hpp"
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
    std::map<std::string, uint64_t> hit_counts() const { return state_index_.page_hit_count_summary(); }

    std::string digest() const;
    std::vector<HiCacheStateTransition> apply_fact(const HiCacheFact & fact, HiCacheSummary & summary);
    std::vector<HiCacheStateTransition> finalize(HiCacheSummary & summary);

  private:
    HiCacheConfig config_;
    HiCacheTargetPager target_pager_;
    HiCacheTokenPathStore token_store_;
    HiCacheTokenRadixTree radix_tree_;
    HiCacheStateIndex state_index_;
    std::unordered_map<std::string, std::vector<std::string>> pending_prefetch_pages_;
    std::unordered_map<std::string, uint64_t> prefetch_decision_ts_;

    uint64_t page_size_for_fact(const HiCacheFact & fact) const;
    std::vector<std::string> pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;
    std::string scoped_request_key(const HiCacheFact & fact) const;
    std::set<std::string> * tier_set(const std::string & tier);
    const std::set<std::string> * tier_set(const std::string & tier) const;
    std::vector<std::string> * touch_order_for_tier(const std::string & tier);
    uint64_t capacity_for_tier(const std::string & tier) const;
    uint64_t target_write_through_threshold() const;
    bool target_write_count_enabled() const;

    void apply_request_tokens(const HiCacheFact & fact);
    void apply_lookup_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_insert_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_capacity_request(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_lock_scope_delta(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_write_policy_hit_counts(const HiCacheFact & fact, const std::vector<std::string> & full_pages, HiCacheSummary & summary,
                                       std::vector<HiCacheStateTransition> & transitions);

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
