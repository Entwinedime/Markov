#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

/**
 * @brief request token path 的完整性等级。
 *
 * 显式记录完整性可以把“输入缺 token”与“请求不足一个 page”区分开，避免模型静默
 * 少生成 target pages。
 */
enum class HiCacheTokenCompleteness { Unknown, Partial, PageAligned, Full };

/**
 * @brief request key 到 token path 的当前最完整映射。
 */
struct HiCacheRequestTokenPath {
    std::string cache_scope;
    std::string request_id;
    HiCacheTokenPath tokens;
    HiCacheTokenCompleteness completeness = HiCacheTokenCompleteness::Unknown;
    std::vector<size_t> source_event_indices;
};

/**
 * @brief request 维度的 token path 表。
 *
 * 表只保存 token path，不保存 page residency、lookup result 或 source movement。
 */
class HiCacheTokenPathStore {
public:
    /** @brief 返回 scope/request 复合 key；缺少 request_id 时返回空字符串。 */
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;

    /** @brief 保存 request token path；更完整或更长的 path 可以覆盖旧值。 */
    void set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens, HiCacheTokenCompleteness completeness);

    /** @brief 记录 request-bound token anchor。 */
    void observe_request_bound_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens);

    /** @brief 查询 request 当前已知 token path。 */
    [[nodiscard]] HiCacheTokenPath request_tokens(const HiCacheFact & fact) const;

    /** @brief 查询 request 当前完整 token path 记录。 */
    [[nodiscard]] const HiCacheRequestTokenPath * request_path(const HiCacheFact & fact) const;

private:
    std::unordered_map<std::string, HiCacheRequestTokenPath> paths_by_request_;
};

} // namespace TraceGraph
