/**
 * @file
 * @brief HiCache request token path 暂存器。
 */
#include "trace_graph/modules/hicache/hicache_token_store.hpp"

#include <algorithm>
#include <ranges>

namespace TraceGraph {

namespace {

/** @brief 比较两个 token path 是否逐 token 完全一致。 */
bool tokens_equal(const HiCacheTokenPath & left, const HiCacheTokenPath & right) {
    return std::ranges::equal(left, right, {}, &HiCacheToken::words, &HiCacheToken::words);
}

/** @brief 空 cache_scope 归一到 -1，保持与 fact parser / pager 一致。 */
std::string normalized_scope(const HiCacheFact & fact) { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

} // namespace

/** @brief 构造 scope/request 复合 key；缺少 request_id 时不登记状态。 */
std::string HiCacheTokenPathStore::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

/**
 * @brief 保存 request token path。
 *
 * 如果同一 request 已有更长 path，则保留更完整的版本，避免较短 fact 覆盖已经可用于
 * target page projection 的上下文。
 */
void HiCacheTokenPathStore::set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) {
    const auto key = scoped_request_key(fact);
    if (key.empty() || tokens.empty()) return;
    auto existing = request_tokens_by_key_.find(key);
    if (existing != request_tokens_by_key_.end() && existing->second.size() >= tokens.size()) return;
    request_tokens_by_key_[key] = tokens;
}

/** @brief 记录 request-bound token anchor，并按 scope/path 去重。 */
void HiCacheTokenPathStore::observe_request_bound_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) {
    if (fact.request_id.empty() || tokens.empty()) return;
    const RequestBoundAnchor anchor{ normalized_scope(fact), tokens };
    auto duplicate = std::ranges::find_if(request_bound_anchors_, [&](const auto & existing) {
        return existing.scope == anchor.scope && tokens_equal(existing.tokens, anchor.tokens);
    });
    if (duplicate != request_bound_anchors_.end()) return;
    request_bound_anchors_.push_back(anchor);
}

/** @brief 查询 request 当前最完整的 token path。 */
HiCacheTokenPath HiCacheTokenPathStore::request_tokens(const HiCacheFact & fact) const {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return {};
    auto it = request_tokens_by_key_.find(key);
    if (it == request_tokens_by_key_.end()) return {};
    return it->second;
}

} // namespace TraceGraph
