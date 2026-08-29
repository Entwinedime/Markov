/**
 * @file
 * @brief HiCache radix-node split policy implementation.
 */
#include "markov/trace_graph/modules/hicache/radix/node_split_policy.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace markov::trace_graph::modules::hicache::radix {


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


void HiCacheNodeSplitPolicy::apply_suffix(HiCacheCacheNode & child, const HiCacheNodeSplitPlan & plan) const { child.pages = plan.suffix_pages; }

} // namespace markov::trace_graph::modules::hicache::radix
