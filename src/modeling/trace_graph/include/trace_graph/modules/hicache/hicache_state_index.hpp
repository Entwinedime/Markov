#pragma once

#include "trace_graph/modules/hicache/hicache_async_state.hpp"
#include "trace_graph/modules/hicache/hicache_storage_directory.hpp"
#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief final-state 派生口径。
 */
enum class HiCacheDerivedStateMode { MaterializedOnly, StorageDirectoryInclusive };

/** @brief 返回派生口径的稳定名称。 */
[[nodiscard]] std::string hicache_derived_state_mode_name(HiCacheDerivedStateMode mode);

/**
 * @brief HiCache final-state 的派生快照。
 *
 * 这些集合不是状态源；它们由 canonical radix node residency/ref 和 async operation
 * table 收集得到，只用于 summary/validation。
 */
struct HiCacheDerivedStateSnapshot {
    HiCacheDerivedStateMode mode = HiCacheDerivedStateMode::MaterializedOnly;
    std::set<std::string> l1;
    std::set<std::string> l2;
    std::set<std::string> l3;
    std::set<std::string> dirty;
    std::set<std::string> backuped;
    std::set<std::string> evicted;
    std::set<std::string> locked;
    std::set<std::string> pending_writeback;
    std::set<std::string> prefetch_planned;
    std::set<std::string> prefetch_ready;
    std::set<std::string> prefetch_late;
    std::set<std::string> prefetch_suppressed;
    std::map<std::string, uint64_t> page_hit_counts;

    [[nodiscard]] std::string digest() const;
};

/**
 * @brief 只读派生视图构建器。
 */
class HiCacheDerivedStateView {
public:
    explicit HiCacheDerivedStateView(HiCacheDerivedStateMode mode = HiCacheDerivedStateMode::MaterializedOnly);

    void include_tree(const HiCacheTokenRadixTree & tree);
    void include_async(const HiCacheAsyncOperationTable & async_ops);
    void include_storage_directory(const HiCacheStorageDirectory & storage);
    [[nodiscard]] HiCacheDerivedStateSnapshot snapshot() const { return snapshot_; }

private:
    HiCacheDerivedStateSnapshot snapshot_;
};

/** @brief 将 set 转成稳定排序 vector。 */
[[nodiscard]] std::vector<std::string> hicache_sorted_vector(const std::set<std::string> & values);

} // namespace TraceGraph
