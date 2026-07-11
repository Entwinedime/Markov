/**
 * @file
 * @brief HiCache radix-node split policy implementation.
 */
#include "markov/trace_graph/modules/hicache/radix/node_split_policy.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace markov::trace_graph::modules::hicache::radix {

#ifdef DEBUG
std::vector<std::string> HiCacheNodeSplitPolicy::owner_keys(const std::map<std::string, uint64_t> & owners) {
    std::vector<std::string> keys;
    keys.reserve(owners.size());
    std::ranges::transform(owners, std::back_inserter(keys), [](const auto & item) { return item.first; });
    return keys;
}
#endif

HiCacheNodeSplitPlan HiCacheNodeSplitPolicy::plan(HiCacheNodeId parent, const HiCacheCacheNode & child, HiCacheNodeId prefix_node_id,
                                                  const HiCacheNodeSplitPages & pages) const {
    // Copying residency, references, and hit count into the prefix is required before the
    // reference ledger and capacity index can re-evaluate the split topology.
    HiCacheCacheNode prefix_node{
        .id = prefix_node_id,
        .parent = parent,
        .children = {},
        .pages = pages.prefix,
        .priority = child.priority,
        .last_access_order = child.last_access_order,
        .active = child.active,
        .residency = child.residency,
        .refs = child.refs,
        .hit_count = child.hit_count,
    };

    return HiCacheNodeSplitPlan{
        .prefix_node = std::move(prefix_node),
        .suffix_pages = pages.suffix,
    };
}

#ifdef DEBUG
void HiCacheNodeSplitPolicy::attach_debug_record(HiCacheNodeSplitPlan & plan, HiCacheNodeId parent, const HiCacheCacheNode & child, size_t split_pages,
                                                 const HiCacheNodeSplitContext & context) const {
    plan.record = HiCacheNodeSplitRecord{
        .parent_node = parent,
        .prefix_node = plan.prefix_node.id,
        .suffix_node = child.id,
        .split_pages = split_pages,
        .parent_child_key = context.parent_child_key,
        .suffix_child_key = context.suffix_child_key,
        .prefix_pages = plan.prefix_node.pages,
        .suffix_pages = plan.suffix_pages,
        .prefix_projection = context.prefix_projection,
        .suffix_projection = context.suffix_projection,
        .prefix_residency = plan.prefix_node.residency,
        .suffix_residency = child.residency,
        .copied_lock_ref_total = child.refs.lock_ref_total,
        .copied_host_ref_total = child.refs.host_ref_total,
        .copied_lock_ref_owners = owner_keys(child.refs.lock_refs_by_owner),
        .copied_host_ref_owners = owner_keys(child.refs.host_refs_by_owner),
        .inherited_hit_count = child.hit_count,
    };
}
#endif

void HiCacheNodeSplitPolicy::apply_suffix(HiCacheCacheNode & child, const HiCacheNodeSplitPlan & plan) const { child.pages = plan.suffix_pages; }

} // namespace markov::trace_graph::modules::hicache::radix
