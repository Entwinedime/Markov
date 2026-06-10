#include "trace_graph/modules/hicache/hicache_token_store.hpp"

namespace TraceGraph {

std::string HiCacheTokenPathStore::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return (fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope) + ":" + fact.request_id;
}

void HiCacheTokenPathStore::set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) {
    const auto key = scoped_request_key(fact);
    if (key.empty() || tokens.empty()) return;
    request_tokens_by_key_[key] = tokens;
}

HiCacheTokenPath HiCacheTokenPathStore::request_tokens(const HiCacheFact & fact) const {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return {};
    auto it = request_tokens_by_key_.find(key);
    if (it == request_tokens_by_key_.end()) return {};
    return it->second;
}

} // namespace TraceGraph
