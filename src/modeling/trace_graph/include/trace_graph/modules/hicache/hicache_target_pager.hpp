#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief target page 的可追溯投影结果。
 *
 * `id` 是模型内部和 summary 使用的稳定 page id；其余字段用于解释该 id 从哪个
 * cache scope 和 token span 推导出来。
 */
struct HiCacheProjectedPage {
    std::string id;
    std::string cache_scope;
    std::string hash;
    uint64_t page_index = 0;
    uint64_t token_begin = 0;
    uint64_t token_end = 0;
};

/**
 * @brief 一条 request path 在 target page size 下的 page 投影。
 */
struct HiCachePagePath {
    std::string cache_scope;
    uint64_t page_size = 0;
    std::vector<HiCacheProjectedPage> pages;

    [[nodiscard]] bool empty() const { return pages.empty(); }
    [[nodiscard]] size_t size() const { return pages.size(); }
    [[nodiscard]] std::vector<std::string> page_ids() const;
};

/**
 * @brief 将 token path 纯函数式投影成 target page id。
 *
 * 该层是 source page identity 与 target state 的隔离层。page id 由 token、
 * chained hash、cache scope 和 target page size 推导，不读取 source residency。
 */
class HiCacheTargetPager {
public:
    explicit HiCacheTargetPager(HiCacheConfig config = HiCacheConfig{});

    /** @brief 返回 fact 应使用的 target page size。 */
    [[nodiscard]] uint64_t page_size_for_fact(const HiCacheFact & fact) const;

    /** @brief 生成包含 cache_scope 的内部 page id。 */
    [[nodiscard]] std::string scoped_page_id(const std::string & cache_scope, const std::string & page_hash) const;

    /** @brief 将 token path 投影成 page path，只保留完整 page。 */
    [[nodiscard]] HiCachePagePath project(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;

    /** @brief 兼容旧调用点的 page id 序列投影。 */
    [[nodiscard]] std::vector<std::string> pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;

private:
    HiCacheConfig config_;
};

} // namespace TraceGraph
