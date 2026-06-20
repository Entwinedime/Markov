/**
 * @file
 * @brief HiCache canonical state 的派生 final-state view。
 */
#include "trace_graph/modules/hicache/hicache_state_index.hpp"

#include <algorithm>
#include <ranges>
#include <sstream>

namespace TraceGraph {

namespace {

void insert_all(std::set<std::string> & target, const std::vector<std::string> & pages) { std::ranges::copy(pages, std::inserter(target, target.end())); }

std::string join_set(const std::set<std::string> & values) {
    std::ostringstream os;
    bool first = true;
    std::ranges::for_each(values, [&](const auto & value) {
        if (!first) os << ",";
        first = false;
        os << value;
    });
    return os.str();
}

} // namespace

std::string hicache_derived_state_mode_name(HiCacheDerivedStateMode mode) {
    switch (mode) {
    case HiCacheDerivedStateMode::MaterializedOnly:
        return "materialized_only";
    case HiCacheDerivedStateMode::StorageDirectoryInclusive:
        return "storage_directory_inclusive";
    }
    return "unknown";
}

std::string HiCacheDerivedStateSnapshot::digest() const {
    std::ostringstream os;
    os << "mode=" << hicache_derived_state_mode_name(mode) << ";l1=" << join_set(l1) << ";l2=" << join_set(l2) << ";l3=" << join_set(l3)
       << ";dirty=" << join_set(dirty) << ";backuped=" << join_set(backuped) << ";evicted=" << join_set(evicted) << ";locked=" << join_set(locked)
       << ";pending_writeback=" << join_set(pending_writeback) << ";prefetch_planned=" << join_set(prefetch_planned)
       << ";prefetch_ready=" << join_set(prefetch_ready) << ";prefetch_late=" << join_set(prefetch_late)
       << ";prefetch_suppressed=" << join_set(prefetch_suppressed);
    return os.str();
}

HiCacheDerivedStateView::HiCacheDerivedStateView(HiCacheDerivedStateMode mode) { snapshot_.mode = mode; }

void HiCacheDerivedStateView::include_tree(const HiCacheTokenRadixTree & tree) {
    for (const auto & node : tree.nodes()) {
        if (!node.active || node.id == 0 || node.pages.empty()) continue;
        const auto backuped = node.residency.host_present || node.residency.storage_readable;
        if (node.residency.device_present) insert_all(snapshot_.l1, node.pages);
        if (node.residency.host_present && node.residency.host_visible) insert_all(snapshot_.l2, node.pages);
        if (node.residency.storage_readable) insert_all(snapshot_.l3, node.pages);
        if (node.residency.device_present && node.residency.device_dirty) insert_all(snapshot_.dirty, node.pages);
        if (backuped) insert_all(snapshot_.backuped, node.pages);
        if (!node.residency.device_present && backuped) insert_all(snapshot_.evicted, node.pages);
        if (node.refs.lock_ref_total > 0 || node.refs.host_ref_total > 0) insert_all(snapshot_.locked, node.pages);
        if (node.hit_count > 0)
            std::ranges::for_each(node.pages,
                                  [&](const auto & page) { snapshot_.page_hit_counts[page] = std::max(snapshot_.page_hit_counts[page], node.hit_count); });
    }
}

void HiCacheDerivedStateView::include_storage_directory(const HiCacheStorageDirectory & storage) {
    if (snapshot_.mode != HiCacheDerivedStateMode::StorageDirectoryInclusive) return;
    for (const auto & page : storage.readable_page_ids(true)) {
        snapshot_.l3.insert(page);
        snapshot_.backuped.insert(page);
        if (!snapshot_.l1.contains(page)) snapshot_.evicted.insert(page);
    }
}

void HiCacheDerivedStateView::include_async(const HiCacheAsyncOperationTable & async_ops) {
    for (const auto & op : async_ops.prefetch_ops() | std::views::values) {
        insert_all(snapshot_.prefetch_planned, op.planned_pages);
        if (op.prefetch_state == HiCachePrefetchState::Ready || op.prefetch_state == HiCachePrefetchState::Applied)
            insert_all(snapshot_.prefetch_ready, op.hit_pages);
        if (op.prefetch_state == HiCachePrefetchState::Late) insert_all(snapshot_.prefetch_late, op.planned_pages);
        if (op.prefetch_state == HiCachePrefetchState::Suppressed || op.prefetch_state == HiCachePrefetchState::Revoked)
            insert_all(snapshot_.prefetch_suppressed, op.planned_pages);
    }
    for (const auto & op : async_ops.writeback_ops() | std::views::values) {
        if (op.header.state != HiCacheOperationState::Queued && op.header.state != HiCacheOperationState::Completed) continue;
        insert_all(snapshot_.pending_writeback, op.header.pages);
    }
}

std::vector<std::string> hicache_sorted_vector(const std::set<std::string> & values) { return { values.begin(), values.end() }; }

} // namespace TraceGraph
