/**
 * @file
 * @brief 从 HiCache runtime 结构派生 final state snapshot 的索引。
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"
#include "markov/trace_graph/modules/hicache/storage/storage_directory.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

using radix::HiCacheTokenRadixTree;
using storage::HiCacheStorageDirectory;

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

    /** @brief 生成当前派生 state snapshot 的稳定摘要。 */
    [[nodiscard]] std::string digest() const;
};

/**
 * @brief 只读派生视图构建器。
 */
class HiCacheDerivedStateView {
public:
    explicit HiCacheDerivedStateView(HiCacheDerivedStateMode mode = HiCacheDerivedStateMode::MaterializedOnly);

    /** @brief 从 canonical radix tree 汇总 materialized residency/ref 状态。 */
    void include_tree(const HiCacheTokenRadixTree & tree);

    /** @brief 从 async operation table 汇总 pending/ready prefetch 和 writeback 状态。 */
    void include_async(const HiCacheAsyncOperationTable & async_ops);

    /** @brief 从 storage directory 汇总 L3 readable/backend 状态。 */
    void include_storage_directory(const HiCacheStorageDirectory & storage);

    /** @brief 返回当前累计出的派生快照。 */
    [[nodiscard]] HiCacheDerivedStateSnapshot snapshot() const { return snapshot_; }

private:
    HiCacheDerivedStateSnapshot snapshot_;
};

/** @brief 将 set 转成稳定排序 vector。 */
[[nodiscard]] std::vector<std::string> hicache_sorted_vector(const std::set<std::string> & values);

} // namespace markov::trace_graph::modules::hicache::runtime
