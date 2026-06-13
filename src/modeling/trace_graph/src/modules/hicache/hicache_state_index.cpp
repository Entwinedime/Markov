/**
 * @file
 * @brief HiCache page state 的扁平集合索引。
 */
#include "trace_graph/modules/hicache/hicache_state_index.hpp"

#include <algorithm>
#include <sstream>

namespace TraceGraph {

namespace {

/** @brief 以稳定顺序拼接 set，用于 digest。 */
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

/** @brief 以稳定 key 顺序拼接计数 map，用于 digest。 */
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

/**
 * @brief 返回包含所有 final-state 集合和 hit count 的确定性 digest。
 *
 * digest 只用于审计 transition 前后状态，不参与状态机决策。
 */
std::string HiCacheStateIndex::digest() const {
    std::ostringstream os;
    os << "l1=" << join_set(l1_) << ";l2=" << join_set(l2_) << ";l3=" << join_set(l3_) << ";dirty=" << join_set(dirty_) << ";backuped=" << join_set(backuped_)
       << ";evicted=" << join_set(evicted_) << ";prefetch_planned=" << join_set(prefetch_planned_) << ";prefetch_ready=" << join_set(prefetch_ready_)
       << ";prefetch_late=" << join_set(prefetch_late_) << ";prefetch_suppressed=" << join_set(prefetch_suppressed_) << ";locked=" << join_set(locked_)
       << ";hit_count=" << join_count_map(hit_count_by_scope_page_);
    return os.str();
}

/** @brief 汇总 page hit count，跨 scope 时保留同一 page 的最大计数。 */
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

/** @brief 返回可写 tier set；未知 tier 返回 nullptr。 */
std::set<std::string> * HiCacheStateIndex::tier_set(const std::string & tier) {
    if (tier == "L1") return &l1_;
    if (tier == "L2") return &l2_;
    if (tier == "L3") return &l3_;
    return nullptr;
}

/** @brief 返回只读 tier set；未知 tier 返回 nullptr。 */
const std::set<std::string> * HiCacheStateIndex::tier_set(const std::string & tier) const {
    if (tier == "L1") return &l1_;
    if (tier == "L2") return &l2_;
    if (tier == "L3") return &l3_;
    return nullptr;
}

/** @brief 返回 tier 对应的 LRU touch order；当前只有 L1/L2 需要 capacity order。 */
std::vector<std::string> * HiCacheStateIndex::touch_order_for_tier(const std::string & tier) {
    if (tier == "L1") return &l1_touch_order_;
    if (tier == "L2") return &l2_touch_order_;
    return nullptr;
}

/** @brief 将 page 移到 tier touch order 末尾，表示最近访问。 */
void HiCacheStateIndex::touch_page(const std::string & tier, const std::string & page) {
    auto * order = touch_order_for_tier(tier);
    if (!order) return;
    order->erase(std::remove(order->begin(), order->end(), page), order->end());
    order->push_back(page);
}

/** @brief 添加 resident page，并维护 touch order 与 L1 re-entry 的 evicted 互斥。 */
bool HiCacheStateIndex::add_resident(const std::string & tier, const std::string & page) {
    auto * pages = tier_set(tier);
    if (!pages) return false;
    touch_page(tier, page);
    const auto inserted = pages->insert(page).second;
    if (inserted && tier == "L1") evicted_.erase(page);
    return inserted;
}

/** @brief 从 resident set 中移除 page；touch order 由上层 cleanup 流程同步维护。 */
bool HiCacheStateIndex::remove_resident(const std::string & tier, const std::string & page) {
    auto * pages = tier_set(tier);
    if (!pages) return false;
    return pages->erase(page) > 0;
}

/** @brief 标记 page dirty。 */
bool HiCacheStateIndex::mark_dirty(const std::string & page) { return dirty_.insert(page).second; }

/** @brief 清除 page dirty 标记。 */
bool HiCacheStateIndex::clear_dirty(const std::string & page) { return dirty_.erase(page) > 0; }

/** @brief 标记 page 有 host/storage 副本。 */
bool HiCacheStateIndex::mark_backuped(const std::string & page) { return backuped_.insert(page).second; }

/** @brief 清除 page 的 backuped 标记。 */
bool HiCacheStateIndex::clear_backuped(const std::string & page) { return backuped_.erase(page) > 0; }

/** @brief 标记 page 已从 device 侧 evicted。 */
bool HiCacheStateIndex::mark_evicted(const std::string & page) { return evicted_.insert(page).second; }

/** @brief 清除 page evicted 标记。 */
bool HiCacheStateIndex::clear_evicted(const std::string & page) { return evicted_.erase(page) > 0; }

/** @brief 标记 page 受 request lock 保护。 */
bool HiCacheStateIndex::mark_locked(const std::string & page) { return locked_.insert(page).second; }

/** @brief 清除 page request lock 保护。 */
bool HiCacheStateIndex::clear_locked(const std::string & page) { return locked_.erase(page) > 0; }

/** @brief 标记 prefetch planned。 */
bool HiCacheStateIndex::mark_prefetch_planned(const std::string & page) { return prefetch_planned_.insert(page).second; }

/** @brief 标记 prefetch ready，并清除互斥的 late/suppressed 状态。 */
bool HiCacheStateIndex::mark_prefetch_ready(const std::string & page) {
    const auto inserted = prefetch_ready_.insert(page).second;
    prefetch_late_.erase(page);
    prefetch_suppressed_.erase(page);
    return inserted;
}

/** @brief 标记 prefetch late；ready page 不会被降级为 late。 */
bool HiCacheStateIndex::mark_prefetch_late(const std::string & page) {
    if (prefetch_ready_.count(page) > 0) return false;
    return prefetch_late_.insert(page).second;
}

/** @brief 标记 prefetch suppressed；ready page 不会被降级为 suppressed。 */
bool HiCacheStateIndex::mark_prefetch_suppressed(const std::string & page) {
    if (prefetch_ready_.count(page) > 0) return false;
    return prefetch_suppressed_.insert(page).second;
}

/** @brief 构造 scope/page 复合 key。 */
std::string HiCacheStateIndex::scoped_page_key(const std::string & cache_scope, const std::string & page) const {
    return (cache_scope.empty() ? std::string("-1") : cache_scope) + ":" + page;
}

/** @brief 增加 scope-local hit count 并返回新值。 */
uint64_t HiCacheStateIndex::increment_hit_count(const std::string & cache_scope, const std::string & page) {
    const auto key = scoped_page_key(cache_scope, page);
    hit_count_by_scope_page_[key]++;
    return hit_count_by_scope_page_[key];
}

/** @brief 增加 scope-local lock count。 */
void HiCacheStateIndex::increment_lock_count(const std::string & cache_scope, const std::string & page) {
    lock_count_by_scope_page_[scoped_page_key(cache_scope, page)]++;
}

/** @brief 减少 scope-local lock count，并返回 page 是否已不再被任何 scope 锁住。 */
bool HiCacheStateIndex::decrement_lock_count(const std::string & cache_scope, const std::string & page) {
    const auto key = scoped_page_key(cache_scope, page);
    auto it = lock_count_by_scope_page_.find(key);
    if (it != lock_count_by_scope_page_.end() && it->second > 0) {
        it->second--;
        if (it->second == 0) lock_count_by_scope_page_.erase(it);
    }
    return !page_locked_in_any_scope(page);
}

/** @brief 检查 page 在任意 scope 下是否仍有 lock count。 */
bool HiCacheStateIndex::page_locked_in_any_scope(const std::string & page) const {
    const auto suffix = ":" + page;
    for (const auto & [key, count] : lock_count_by_scope_page_) {
        if (count > 0 && key.size() >= suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
}

} // namespace TraceGraph
