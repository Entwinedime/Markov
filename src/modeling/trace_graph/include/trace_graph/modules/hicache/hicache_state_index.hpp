#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

class HiCacheStateIndex {
  public:
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

    std::string digest() const;
    std::map<std::string, uint64_t> page_hit_count_summary() const;

    std::set<std::string> * tier_set(const std::string & tier);
    const std::set<std::string> * tier_set(const std::string & tier) const;
    std::vector<std::string> * touch_order_for_tier(const std::string & tier);
    void touch_page(const std::string & tier, const std::string & page);

    bool add_resident(const std::string & tier, const std::string & page);
    bool remove_resident(const std::string & tier, const std::string & page);
    bool mark_dirty(const std::string & page);
    bool clear_dirty(const std::string & page);
    bool mark_backuped(const std::string & page);
    bool clear_backuped(const std::string & page);
    bool mark_evicted(const std::string & page);
    bool clear_evicted(const std::string & page);
    bool mark_locked(const std::string & page);
    bool clear_locked(const std::string & page);
    bool mark_prefetch_planned(const std::string & page);
    bool mark_prefetch_ready(const std::string & page);
    bool mark_prefetch_late(const std::string & page);
    bool mark_prefetch_suppressed(const std::string & page);

    uint64_t increment_hit_count(const std::string & cache_scope, const std::string & page);
    void increment_lock_count(const std::string & cache_scope, const std::string & page);
    bool decrement_lock_count(const std::string & cache_scope, const std::string & page);
    bool page_locked_in_any_scope(const std::string & page) const;

  private:
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
    std::unordered_map<std::string, uint64_t> hit_count_by_scope_page_;
    std::unordered_map<std::string, uint64_t> lock_count_by_scope_page_;
    std::vector<std::string> l1_touch_order_;
    std::vector<std::string> l2_touch_order_;

    std::string scoped_page_key(const std::string & cache_scope, const std::string & page) const;
};

} // namespace TraceGraph
