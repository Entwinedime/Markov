#pragma once

#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief split policy 所需的 topology/projection 上下文。
 */
struct HiCacheNodeSplitContext {
    std::string parent_child_key;
    std::string suffix_child_key;
    HiCacheNodeSplitProjection prefix_projection;
    HiCacheNodeSplitProjection suffix_projection;
};

/**
 * @brief radix split 的字段继承计划。
 */
struct HiCacheNodeSplitPlan {
    HiCacheCacheNode prefix_node;
    HiCacheNodeSplitRecord record;
};

/**
 * @brief HiCache canonical radix node split 策略。
 *
 * SGLang split 会把 child 的 value/host value 按 prefix/suffix 切开；lock_ref
 * 和 hit_count 则保持 node-chain 语义，prefix node 继承一份，suffix child 保留一份。
 * 本策略把这些字段规则从 tree topology 操作中拆出来。
 */
class HiCacheNodeSplitPolicy {
public:
    /** @brief 构造 prefix node 和审计记录；不修改原 child。 */
    [[nodiscard]] HiCacheNodeSplitPlan plan(HiCacheNodeId parent, const HiCacheCacheNode & child, HiCacheNodeId prefix_node_id, size_t split_pages,
                                            const std::vector<std::string> & prefix_pages, const std::vector<std::string> & suffix_pages,
                                            const HiCacheNodeSplitContext & context) const;

    /** @brief 将 suffix child 调整为 split 后状态；保留 residency/ref/hit_count。 */
    void apply_suffix(HiCacheCacheNode & child, const HiCacheNodeSplitPlan & plan) const;

private:
    [[nodiscard]] static std::vector<std::string> owner_keys(const std::map<std::string, uint64_t> & owners);
};

} // namespace TraceGraph
