#pragma once

#include "trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace TraceGraph {

class TraceDAG;

struct CacheIOSummary {
    uint64_t events = 0;
    uint64_t transfer_events = 0;
    uint64_t eviction_events = 0;
    uint64_t writeback_events = 0;
    uint64_t events_with_tokens = 0;
    uint64_t events_with_pages = 0;
    uint64_t events_with_page_size = 0;
    uint64_t events_with_bytes = 0;
    uint64_t missing_bytes_events = 0;
    uint64_t estimated_latency_us = 0;
    std::map<std::string, uint64_t> hit_tokens_by_tier;
    std::map<std::string, uint64_t> bytes_by_edge;
    std::map<std::string, int64_t> resident_pages_by_tier;

    std::string to_json() const;
    void write_json(const std::string & filename) const;
};

CacheIOSummary apply_cache_io_model(TraceDAG & dag, const CacheIOConfig & config);

} // namespace TraceGraph
