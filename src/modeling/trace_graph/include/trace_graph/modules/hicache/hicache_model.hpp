#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_fact.hpp"
#include "trace_graph/modules/hicache/hicache_radix_tree.hpp"
#include "trace_graph/modules/hicache/hicache_summary.hpp"

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

    const std::set<std::string> & l1() const { return l1_; }
    const std::set<std::string> & l2() const { return l2_; }
    const std::set<std::string> & l3() const { return l3_; }
    const std::set<std::string> & dirty() const { return dirty_; }
    const std::set<std::string> & backuped() const { return backuped_; }
    const std::set<std::string> & evicted() const { return evicted_; }
    const std::set<std::string> & locked() const { return locked_; }
    const std::set<std::string> & prefetch_planned() const { return prefetch_planned_; }
    const std::set<std::string> & prefetch_ready() const { return prefetch_ready_; }
    const std::set<std::string> & prefetch_late() const { return prefetch_late_; }
    const std::set<std::string> & prefetch_suppressed() const { return prefetch_suppressed_; }
    std::map<std::string, uint64_t> hit_counts() const { return page_hit_count_summary(); }

    std::string digest() const;
    std::vector<HiCacheStateTransition> apply_fact(const HiCacheFact & fact, HiCacheSummary & summary);
    std::vector<HiCacheStateTransition> finalize(HiCacheSummary & summary);

  private:
    HiCacheConfig config_;
    std::set<std::string> l1_;
    std::set<std::string> l2_;
    std::set<std::string> l3_;
    std::set<std::string> dirty_;
    std::set<std::string> backuped_;
    std::set<std::string> evicted_;
    std::set<std::string> locked_;
    std::set<std::string> prefetch_planned_;
    std::set<std::string> prefetch_ready_;
    std::set<std::string> prefetch_late_;
    std::set<std::string> prefetch_suppressed_;
    std::unordered_map<std::string, std::vector<std::string>> request_pages_;
    std::unordered_map<std::string, std::vector<std::string>> pending_prefetch_pages_;
    std::unordered_map<std::string, uint64_t> prefetch_intent_ts_;
    std::unordered_map<std::string, uint64_t> hit_count_by_scope_page_;
    std::unordered_map<std::string, uint64_t> lock_count_by_scope_page_;
    std::vector<std::string> l1_touch_order_;
    std::vector<std::string> l2_touch_order_;
    HiCacheRadixTree radix_tree_;

    uint64_t page_size_for_fact(const HiCacheFact & fact) const;
    std::vector<std::string> pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;
    std::vector<std::string> suffix_pages_for_prefetch(const HiCacheFact & fact) const;
    std::string scoped_request_key(const HiCacheFact & fact) const;
    std::string scoped_page_key(const HiCacheFact & fact, const std::string & page) const;
    std::string scoped_page_id(const HiCacheFact & fact, const std::string & page_hash) const;
    std::set<std::string> * tier_set(const std::string & tier);
    const std::set<std::string> * tier_set(const std::string & tier) const;
    std::vector<std::string> * touch_order_for_tier(const std::string & tier);
    uint64_t capacity_for_tier(const std::string & tier) const;
    uint64_t target_write_through_threshold() const;
    bool target_write_count_enabled() const;
    bool page_locked_in_any_scope(const std::string & page) const;

    void apply_request_tokens(const HiCacheFact & fact);
    void apply_lookup_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_insert_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_prefetch_intent(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
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
    void clear_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                        const std::string & page);
    void mark_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void clear_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void clear_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page);
    void mark_prefetch_planned(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page);
    void mark_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                             const std::string & page);
    void mark_prefetch_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                            const std::string & page);
    void mark_prefetch_suppressed(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page);
    void touch_page(const std::string & tier, const std::string & page);
    void evict_lru_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                         uint64_t page_count);
    void enforce_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier);
    void flush_dirty_page_to_host(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page);
    std::map<std::string, uint64_t> page_hit_count_summary() const;
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
