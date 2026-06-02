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
    std::string model_config_path = "";
    uint64_t tp_size = 1;
    uint64_t num_layers = 0;
    uint64_t num_kv_heads = 0;
    uint64_t head_dim = 0;
    uint64_t dtype_bytes = 0;
    std::vector<CacheIOTierConfig> tiers;
    std::string page_size_policy = "trace";
    std::string write_policy = "trace";
    bool writeback_on_evict = true;
    std::string prefetch_policy = "trace_replay";
    uint64_t prefetch_threshold = 0;
    double prefetch_timeout_base = 0.0;
    double prefetch_timeout_per_ki_token = 0.0;
    double prefetch_timeout_max = 0.0;
    bool count_async_prefetch_latency = false;
};

struct ModelConfig {
    std::vector<std::string> domains;
    CacheIOConfig cache_io;

    bool domain_enabled(const std::string & name) const;
    static ModelConfig from_file(const std::string & filename);
};

} // namespace TraceGraph
