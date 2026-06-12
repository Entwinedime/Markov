#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

class HiCacheTokenPathStore {
  public:
    std::string scoped_request_key(const HiCacheFact & fact) const;
    void set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens);
    void observe_request_bound_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens);
    HiCacheTokenPath request_tokens(const HiCacheFact & fact) const;

  private:
    struct RequestBoundAnchor {
        std::string scope;
        HiCacheTokenPath tokens;
    };

    std::unordered_map<std::string, HiCacheTokenPath> request_tokens_by_key_;
    std::vector<RequestBoundAnchor> request_bound_anchors_;
};

} // namespace TraceGraph
