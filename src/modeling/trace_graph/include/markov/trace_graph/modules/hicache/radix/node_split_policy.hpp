/**
 * @file
 * @brief Node-split policy for the HiCache token radix tree.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::radix {


/**
 * @brief Field-inheritance plan for one radix split.
 */
struct HiCacheNodeSplitPlan {
    HiCacheCacheNode prefix_node;
    std::vector<std::string> suffix_pages;
};

/** @brief Non-owning page partition supplied to one radix split plan. */
struct HiCacheNodeSplitPages {
    const std::vector<std::string> & prefix;
    const std::vector<std::string> & suffix;
};

/**
 * @brief Canonical HiCache radix-node split policy.
 *
 * SGLang partitions device and host values into prefix and suffix segments. Reference
 * counters and hit count retain node-chain semantics, so the new prefix inherits a copy
 * while the suffix child keeps its original state. This class isolates those rules from
 * topology mutation.
 */
class HiCacheNodeSplitPolicy {
public:
    /** @brief Builds the business split plan without mutating the suffix child. */
    [[nodiscard]] HiCacheNodeSplitPlan plan(HiCacheNodeId parent, const HiCacheCacheNode & child, HiCacheNodeId prefix_node_id,
                                            const HiCacheNodeSplitPages & pages) const;


    /** @brief Replaces suffix pages while preserving residency, references, and hit count. */
    void apply_suffix(HiCacheCacheNode & child, const HiCacheNodeSplitPlan & plan) const;

private:
};

} // namespace markov::trace_graph::modules::hicache::radix
