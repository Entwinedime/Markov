#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <string>
#include <unordered_map>

namespace TraceGraph {

class HiCacheTokenPathStore {
  public:
    std::string scoped_request_key(const HiCacheFact & fact) const;
    void set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens);
    HiCacheTokenPath request_tokens(const HiCacheFact & fact) const;

  private:
    std::unordered_map<std::string, HiCacheTokenPath> request_tokens_by_key_;
};

} // namespace TraceGraph
