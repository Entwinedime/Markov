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
    std::unordered_map<std::string, std::vector<std::string>> pending_lookup_pages_by_request_;
    std::unordered_map<std::string, std::vector<std::string>> pending_prefetch_pages_by_request_;
    std::unordered_map<std::string, std::vector<std::string>> latest_prefetch_schedule_pages_by_request_;
    std::unordered_map<std::string, std::vector<std::string>> latest_prefetch_progress_pages_by_request_;
    std::unordered_map<std::string, uint64_t> prefetch_schedule_ts_by_request_;
    std::unordered_map<std::string, uint64_t> prefetch_schedule_source_page_count_by_request_;
    std::unordered_map<std::string, uint64_t> hit_count_by_scope_page_;
    std::unordered_map<std::string, uint64_t> lock_count_by_scope_page_;
    std::set<std::string> terminated_prefetch_requests_;
    std::set<std::string> prefetch_transfer_resident_credited_requests_;
    std::vector<std::string> last_lookup_pages_;
    std::unordered_map<std::string, std::vector<std::string>> last_lookup_pages_by_scope_;
    std::vector<std::string> l1_touch_order_;
    std::vector<std::string> l2_touch_order_;
    HiCacheRadixTree radix_tree_;
    std::string last_policy_evict_key_;
    uint64_t last_policy_evict_ts_ = 0;

    std::set<std::string> * tier_set(const std::string & tier);
    const std::set<std::string> * tier_set(const std::string & tier) const;
    std::vector<std::string> * touch_order_for_tier(const std::string & tier);
    uint64_t capacity_for_tier(const std::string & tier) const;
    bool target_page_size_mismatch(const HiCacheFact & fact) const;
    bool target_capacity_configured() const;
    bool target_load_model_enabled(const HiCacheFact & fact) const;
    void apply_lookup(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    std::vector<std::string> target_radix_path_for_insert(const HiCacheFact & fact) const;
    std::vector<std::string> target_insert_pages(const HiCacheFact & fact) const;
    bool target_prefix_page_known(const std::string & page) const;
    void apply_radix_removed_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_insert(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    uint64_t missing_resident_page_count(const std::string & tier, const std::vector<std::string> & pages) const;
    uint64_t pages_to_free_for_insert(const std::string & tier, const std::vector<std::string> & pages) const;
    void enforce_insert_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & tier, const std::vector<std::string> & pages);
    uint64_t target_write_through_threshold() const;
    bool target_write_count_enabled() const;
    std::string scoped_request_key(const HiCacheFact & fact) const;
    std::string scoped_page_key(const HiCacheFact & fact, const std::string & page) const;
    bool page_locked_in_any_scope(const std::string & page) const;
    std::vector<std::string> write_policy_hit_pages_for_insert(const HiCacheFact & fact) const;
    bool write_policy_prefix_backup_ready(const std::vector<std::string> & pages, size_t index) const;
    void apply_write_policy_hit_counts(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void backup_page_for_write_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      const std::string & page);
    void apply_load_to_l1(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_l3_to_l2(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, bool prefetch_ready);
    void apply_prefetch_progress(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_write_to_l2(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_write_to_l3(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_evict(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_policy_evict(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_remove_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    void apply_lock_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, bool increment);
    void apply_generic_tier_move(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);

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
    void remember_prefetch_pages(const HiCacheFact & fact, const std::vector<std::string> & pages);
    void remember_prefetch_schedule(const HiCacheFact & fact, const std::vector<std::string> & pages);
    std::vector<std::string> target_prefetch_schedule_pages(const HiCacheFact & fact) const;
    std::vector<std::string> target_prefetch_completion_pages(const HiCacheFact & fact) const;
    std::vector<std::string> prefetch_pages_for_fact(const HiCacheFact & fact) const;
    void finalize_prefetch_policy(HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions);
    bool should_terminate_prefetch_at_progress(const HiCacheFact & fact, const std::vector<std::string> & pages, uint64_t ready_count) const;
    bool prefetch_timeout_reached(const HiCacheFact & fact, const std::vector<std::string> & pages) const;
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

// HiCache 功能模型入口。
// 当前阶段只做状态维护和验证摘要，DAG patch 后续在同一模块内继续扩展。
HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config);

} // namespace TraceGraph
