#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief 从 target 配置和 token path 投影 HiCache page id。
 *
 * pager 是 source page id 与 target page id 的隔离层。模型只使用这里生成的 scoped
 * chained hash page id，因此不会把 source_actual residency 直接写入 target state。
 */
class HiCacheTargetPager {
public:
    explicit HiCacheTargetPager(HiCacheConfig config = HiCacheConfig{});

    /** @brief 返回用于该 fact 的 target page size。 */
    [[nodiscard]] uint64_t page_size_for_fact(const HiCacheFact & fact) const;

    /** @brief 将 page hash 绑定到 cache_scope，避免不同 scope 的 page id 冲突。 */
    [[nodiscard]] std::string scoped_page_id(const HiCacheFact & fact, const std::string & page_hash) const;

    /**
     * @brief 将 token path 切成 page，并为每个 page 生成 chained hash id。
     *
     * 只处理 page-aligned 前缀；不足一个 page 的尾部 token 不形成 cache page。
     */
    [[nodiscard]] std::vector<std::string> pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;

private:
    HiCacheConfig config_;
};

} // namespace TraceGraph
