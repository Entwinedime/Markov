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

std::string normalized_scope(const HiCacheFact & fact) { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

} // namespace

std::string HiCacheTokenPathStore::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

void HiCacheTokenPathStore::set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) {
    const auto key = scoped_request_key(fact);
    if (key.empty() || tokens.empty()) return;
    auto existing = request_tokens_by_key_.find(key);
    if (existing != request_tokens_by_key_.end() && existing->second.size() >= tokens.size()) return;
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

} // namespace TraceGraph
