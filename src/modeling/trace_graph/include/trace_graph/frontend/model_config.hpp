#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

struct CacheIOTierConfig {
    std::string name;
    bool capacity_infer = true;
    bool capacity_infinite = false;
    uint64_t capacity_pages = 0;
    double latency_us = 0.0;
    bool bandwidth_infer = true;
    bool bandwidth_infinite = false;
    double bandwidth_gbps = 0.0;
    std::string eviction = "lru";
};

struct CacheIOConfig {
    bool enabled = false;
    std::string page_size_tokens = "infer";
    std::string bytes_per_page = "infer";
    std::vector<CacheIOTierConfig> tiers;
    std::string write_policy = "trace";
    std::string prefetch_policy = "trace_replay";
};

struct ModelConfig {
    std::vector<std::string> domains;
    CacheIOConfig cache_io;

    bool domain_enabled(const std::string & name) const;
    static ModelConfig from_file(const std::string & filename);
};

} // namespace TraceGraph
