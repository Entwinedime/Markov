#include "trace_graph/modules/hicache/hicache_state_index.hpp"

#include <algorithm>
#include <sstream>

namespace TraceGraph {

namespace {

std::string join_set(const std::set<std::string> & values) {
    std::ostringstream os;
    bool first = true;
    for (const auto & value : values) {
        if (!first) os << ",";
        first = false;
        os << value;
    }
    return os.str();
}

std::string join_count_map(const std::unordered_map<std::string, uint64_t> & values) {
    std::map<std::string, uint64_t> ordered(values.begin(), values.end());
    std::ostringstream os;
    bool first = true;
    for (const auto & [key, count] : ordered) {
        if (!first) os << ",";
        first = false;
        os << key << ":" << count;
    }
    return os.str();
}

} // namespace

std::string HiCacheStateIndex::digest() const {
    std::ostringstream os;
    os << "l1=" << join_set(l1_) << ";l2=" << join_set(l2_) << ";l3=" << join_set(l3_) << ";dirty=" << join_set(dirty_) << ";backuped=" << join_set(backuped_)
       << ";evicted=" << join_set(evicted_) << ";prefetch_planned=" << join_set(prefetch_planned_) << ";prefetch_ready=" << join_set(prefetch_ready_)
       << ";prefetch_late=" << join_set(prefetch_late_) << ";prefetch_suppressed=" << join_set(prefetch_suppressed_) << ";locked=" << join_set(locked_)
       << ";hit_count=" << join_count_map(hit_count_by_scope_page_);
    return os.str();
}

std::map<std::string, uint64_t> HiCacheStateIndex::page_hit_count_summary() const {
    std::map<std::string, uint64_t> result;
    for (const auto & [scoped_key, count] : hit_count_by_scope_page_) {
        const auto separator = scoped_key.find(':');
        const auto page = separator == std::string::npos ? scoped_key : scoped_key.substr(separator + 1);
        auto & current = result[page];
        current = std::max(current, count);
    }
    return result;
}

std::set<std::string> * HiCacheStateIndex::tier_set(const std::string & tier) {
    if (tier == "L1") return &l1_;
    if (tier == "L2") return &l2_;
    if (tier == "L3") return &l3_;
    return nullptr;
}

const std::set<std::string> * HiCacheStateIndex::tier_set(const std::string & tier) const {
    if (tier == "L1") return &l1_;
    if (tier == "L2") return &l2_;
    if (tier == "L3") return &l3_;
    return nullptr;
}

std::vector<std::string> * HiCacheStateIndex::touch_order_for_tier(const std::string & tier) {
    if (tier == "L1") return &l1_touch_order_;
    if (tier == "L2") return &l2_touch_order_;
    return nullptr;
}

void HiCacheStateIndex::touch_page(const std::string & tier, const std::string & page) {
    auto * order = touch_order_for_tier(tier);
    if (!order) return;
    order->erase(std::remove(order->begin(), order->end(), page), order->end());
    order->push_back(page);
}

bool HiCacheStateIndex::add_resident(const std::string & tier, const std::string & page) {
    auto * pages = tier_set(tier);
    if (!pages) return false;
    touch_page(tier, page);
    const auto inserted = pages->insert(page).second;
    if (inserted) evicted_.erase(page);
    return inserted;
}

bool HiCacheStateIndex::remove_resident(const std::string & tier, const std::string & page) {
    auto * pages = tier_set(tier);
    if (!pages) return false;
    return pages->erase(page) > 0;
}

bool HiCacheStateIndex::mark_dirty(const std::string & page) { return dirty_.insert(page).second; }

bool HiCacheStateIndex::clear_dirty(const std::string & page) { return dirty_.erase(page) > 0; }

bool HiCacheStateIndex::mark_backuped(const std::string & page) { return backuped_.insert(page).second; }

bool HiCacheStateIndex::clear_backuped(const std::string & page) { return backuped_.erase(page) > 0; }

bool HiCacheStateIndex::mark_evicted(const std::string & page) { return evicted_.insert(page).second; }

bool HiCacheStateIndex::clear_evicted(const std::string & page) { return evicted_.erase(page) > 0; }

bool HiCacheStateIndex::mark_locked(const std::string & page) { return locked_.insert(page).second; }

bool HiCacheStateIndex::clear_locked(const std::string & page) { return locked_.erase(page) > 0; }

bool HiCacheStateIndex::mark_prefetch_planned(const std::string & page) { return prefetch_planned_.insert(page).second; }

bool HiCacheStateIndex::mark_prefetch_ready(const std::string & page) {
    const auto inserted = prefetch_ready_.insert(page).second;
    prefetch_late_.erase(page);
    prefetch_suppressed_.erase(page);
    return inserted;
}

bool HiCacheStateIndex::mark_prefetch_late(const std::string & page) {
    if (prefetch_ready_.count(page) > 0) return false;
    return prefetch_late_.insert(page).second;
}

bool HiCacheStateIndex::mark_prefetch_suppressed(const std::string & page) {
    if (prefetch_ready_.count(page) > 0) return false;
    return prefetch_suppressed_.insert(page).second;
}

std::string HiCacheStateIndex::scoped_page_key(const std::string & cache_scope, const std::string & page) const {
    return (cache_scope.empty() ? std::string("-1") : cache_scope) + ":" + page;
}

uint64_t HiCacheStateIndex::increment_hit_count(const std::string & cache_scope, const std::string & page) {
    const auto key = scoped_page_key(cache_scope, page);
    hit_count_by_scope_page_[key]++;
    return hit_count_by_scope_page_[key];
}

void HiCacheStateIndex::increment_lock_count(const std::string & cache_scope, const std::string & page) {
    lock_count_by_scope_page_[scoped_page_key(cache_scope, page)]++;
}

bool HiCacheStateIndex::decrement_lock_count(const std::string & cache_scope, const std::string & page) {
    const auto key = scoped_page_key(cache_scope, page);
    auto it = lock_count_by_scope_page_.find(key);
    if (it != lock_count_by_scope_page_.end() && it->second > 0) {
        it->second--;
        if (it->second == 0) lock_count_by_scope_page_.erase(it);
    }
    return !page_locked_in_any_scope(page);
}

bool HiCacheStateIndex::page_locked_in_any_scope(const std::string & page) const {
    const auto suffix = ":" + page;
    for (const auto & [key, count] : lock_count_by_scope_page_) {
        if (count > 0 && key.size() >= suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
}

} // namespace TraceGraph
