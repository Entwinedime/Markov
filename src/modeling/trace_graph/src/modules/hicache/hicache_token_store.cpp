#include "trace_graph/modules/hicache/hicache_token_store.hpp"

#include <algorithm>

namespace TraceGraph {

namespace {

bool tokens_equal(const HiCacheTokenPath & left, const HiCacheTokenPath & right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].words != right[index].words) return false;
    }
    return true;
}

bool is_token_prefix(const HiCacheTokenPath & prefix, const HiCacheTokenPath & full) {
    if (prefix.empty() || prefix.size() > full.size()) return false;
    for (size_t index = 0; index < prefix.size(); ++index) {
        if (prefix[index].words != full[index].words) return false;
    }
    return true;
}

HiCacheTokenPath slice_tokens(const HiCacheTokenPath & tokens, size_t begin, size_t end) {
    if (begin >= tokens.size() || begin >= end) return {};
    end = std::min(end, tokens.size());
    return {tokens.begin() + static_cast<long>(begin), tokens.begin() + static_cast<long>(end)};
}

size_t page_aligned_len(size_t token_count, uint64_t page_size) {
    if (token_count == 0 || page_size == 0) return 0;
    const auto page = static_cast<size_t>(page_size);
    return token_count / page * page;
}

std::string normalized_scope(const HiCacheFact & fact) { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

} // namespace

std::string HiCacheTokenPathStore::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

void HiCacheTokenPathStore::set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) {
    const auto key = scoped_request_key(fact);
    if (key.empty() || tokens.empty()) return;
    request_tokens_by_key_[key] = tokens;
}

void HiCacheTokenPathStore::observe_request_bound_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) {
    if (fact.request_id.empty() || tokens.empty()) return;
    const RequestBoundAnchor anchor{normalized_scope(fact), tokens};
    auto duplicate = std::find_if(request_bound_anchors_.begin(), request_bound_anchors_.end(), [&](const auto & existing) {
        return existing.scope == anchor.scope && tokens_equal(existing.tokens, anchor.tokens);
    });
    if (duplicate != request_bound_anchors_.end()) return;
    request_bound_anchors_.push_back(anchor);
}

HiCacheTokenPath HiCacheTokenPathStore::request_tokens(const HiCacheFact & fact) const {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return {};
    auto it = request_tokens_by_key_.find(key);
    if (it == request_tokens_by_key_.end()) return {};
    return it->second;
}

HiCacheTokenPath HiCacheTokenPathStore::target_cache_stage_tokens(const HiCacheFact & fact, const HiCacheTokenPath & observed_tokens,
                                                                  uint64_t target_page_size) const {
    if (!fact.request_id.empty() || observed_tokens.empty() || target_page_size == 0) return observed_tokens;
    const auto scope = normalized_scope(fact);
    for (auto it = request_bound_anchors_.rbegin(); it != request_bound_anchors_.rend(); ++it) {
        if (it->scope != scope) continue;
        const auto & anchor_tokens = it->tokens;
        if (!is_token_prefix(observed_tokens, anchor_tokens)) continue;
        const auto source_aligned_len = page_aligned_len(anchor_tokens.size(), fact.source_page_size);
        if (!tokens_equal(observed_tokens, anchor_tokens) && observed_tokens.size() != source_aligned_len) continue;
        const auto target_aligned_len = page_aligned_len(anchor_tokens.size(), target_page_size);
        if (target_aligned_len == 0) return observed_tokens;
        return slice_tokens(anchor_tokens, 0, target_aligned_len);
    }
    return observed_tokens;
}

} // namespace TraceGraph
