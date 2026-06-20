/**
 * @file
 * @brief HiCache request token path table。
 */
#include "trace_graph/modules/hicache/hicache_token_store.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace TraceGraph {

namespace {

std::string normalized_scope(const HiCacheFact & fact) { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

uint8_t rank(HiCacheTokenCompleteness completeness) {
    switch (completeness) {
    case HiCacheTokenCompleteness::Unknown:
        return 0;
    case HiCacheTokenCompleteness::Partial:
        return 1;
    case HiCacheTokenCompleteness::PageAligned:
        return 2;
    case HiCacheTokenCompleteness::Full:
        return 3;
    }
    return 0;
}

} // namespace

std::string HiCacheTokenPathStore::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

void HiCacheTokenPathStore::set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens, HiCacheTokenCompleteness completeness) {
    const auto key = scoped_request_key(fact);
    if (key.empty() || tokens.empty()) return;

    auto candidate = HiCacheRequestTokenPath{
        .cache_scope = normalized_scope(fact),
        .request_id = fact.request_id,
        .tokens = tokens,
        .completeness = completeness,
        .source_event_indices = { fact.source_event_index },
    };

    auto it = paths_by_request_.find(key);
    if (it == paths_by_request_.end()) {
        paths_by_request_.emplace(key, std::move(candidate));
        return;
    }

    auto & existing = it->second;
    const bool better_completeness = rank(candidate.completeness) > rank(existing.completeness);
    const bool longer_same_level = rank(candidate.completeness) == rank(existing.completeness) && candidate.tokens.size() > existing.tokens.size();
    existing.source_event_indices.push_back(fact.source_event_index);
    if (better_completeness || longer_same_level) {
        candidate.source_event_indices = existing.source_event_indices;
        existing = std::move(candidate);
    }
}

void HiCacheTokenPathStore::observe_request_bound_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) {
    set_request_tokens(fact, tokens, HiCacheTokenCompleteness::Full);
}

HiCacheTokenPath HiCacheTokenPathStore::request_tokens(const HiCacheFact & fact) const {
    const auto * path = request_path(fact);
    return path == nullptr ? HiCacheTokenPath{} : path->tokens;
}

const HiCacheRequestTokenPath * HiCacheTokenPathStore::request_path(const HiCacheFact & fact) const {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return nullptr;
    const auto it = paths_by_request_.find(key);
    return it == paths_by_request_.end() ? nullptr : &it->second;
}

} // namespace TraceGraph
