#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

class HiCacheTargetPager {
  public:
    explicit HiCacheTargetPager(HiCacheConfig config = HiCacheConfig{});

    uint64_t page_size_for_fact(const HiCacheFact & fact) const;
    std::string scoped_page_id(const HiCacheFact & fact, const std::string & page_hash) const;
    std::vector<std::string> pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;

  private:
    HiCacheConfig config_;
};

} // namespace TraceGraph
