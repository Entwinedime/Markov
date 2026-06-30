/**
 * @file
 * @brief HiCache radix node split policy 实现。
 */
#include "markov/trace_graph/modules/hicache/radix/node_split_policy.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace markov::trace_graph::modules::hicache::radix {

std::vector<std::string> HiCacheNodeSplitPolicy::owner_keys(const std::map<std::string, uint64_t> & owners) {
    std::vector<std::string> keys;
    keys.reserve(owners.size());
    std::ranges::transform(owners, std::back_inserter(keys), [](const auto & item) { return item.first; });
    return keys;
}

HiCacheNodeSplitPlan HiCacheNodeSplitPolicy::plan(HiCacheNodeId parent, const HiCacheCacheNode & child, HiCacheNodeId prefix_node_id, size_t split_pages,
                                                  const std::vector<std::string> & prefix_pages, const std::vector<std::string> & suffix_pages,
                                                  const HiCacheNodeSplitContext & context) const {
    /**
     * @brief split policy 采用“prefix 继承旧 child residency/ref/hit_count”的语义。
     *
     * 这是后续 ref ledger sync 和 capacity index 重新判定 leaf eligibility 的前提。
     */
    HiCacheCacheNode prefix_node{
        .id = prefix_node_id,
        .parent = parent,
        .children = {},
        .pages = prefix_pages,
        .priority = child.priority,
        .last_access_order = child.last_access_order,
        .active = child.active,
        .residency = child.residency,
        .refs = child.refs,
        .hit_count = child.hit_count,
    };

    HiCacheNodeSplitRecord record{
        .parent_node = parent,
        .prefix_node = prefix_node_id,
        .suffix_node = child.id,
        .split_pages = split_pages,
        .parent_child_key = context.parent_child_key,
        .suffix_child_key = context.suffix_child_key,
        .prefix_pages = prefix_pages,
        .suffix_pages = suffix_pages,
        .prefix_projection = context.prefix_projection,
        .suffix_projection = context.suffix_projection,
        .prefix_residency = prefix_node.residency,
        .suffix_residency = child.residency,
        .copied_lock_ref_total = child.refs.lock_ref_total,
        .copied_host_ref_total = child.refs.host_ref_total,
        .copied_lock_ref_owners = owner_keys(child.refs.lock_refs_by_owner),
        .copied_host_ref_owners = owner_keys(child.refs.host_refs_by_owner),
        .inherited_hit_count = child.hit_count,
    };

    return HiCacheNodeSplitPlan{
        .prefix_node = std::move(prefix_node),
        .record = std::move(record),
    };
}

void HiCacheNodeSplitPolicy::apply_suffix(HiCacheCacheNode & child, const HiCacheNodeSplitPlan & plan) const { child.pages = plan.record.suffix_pages; }

} // namespace markov::trace_graph::modules::hicache::radix
