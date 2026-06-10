#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

struct TraceEvent;

struct HiCacheFact {
    size_t source_node_id = 0;
    size_t source_event_index = 0;
    uint64_t ts = 0;
    std::string event_name;
    std::string role;
    std::string request_id;
    std::string operation_id;
    std::string cache_scope;
    std::string tier_src;
    std::string tier_dst;
    std::string direction;
    std::vector<std::string> pages;
    std::vector<std::string> source_pages;
    std::vector<std::string> target_pages;
    std::unordered_map<uint64_t, std::vector<std::string>> target_pages_by_page_size;
    std::vector<std::string> radix_removed_pages;
    std::vector<std::string> target_radix_removed_pages;
    std::unordered_map<uint64_t, std::vector<std::string>> target_radix_removed_pages_by_page_size;
    uint64_t page_size = 0;
    uint64_t prefix_len = 0;
    uint64_t new_input_tokens = 0;
    uint64_t completed_tokens = 0;
    uint64_t requested_tokens = 0;
    uint64_t evicted_tokens = 0;
    uint64_t prefetch_ready_page_count = 0;
    bool is_start = false;
    bool requires_page_identity = false;
    bool dirty = true;
    bool backuped = false;
    bool prefetch_progress_evidence = false;
    bool prefetch_check_available = false;
    bool prefetch_check_return = false;
    bool prefetch_has_ongoing = false;
    bool chunked = false;
    bool radix_removed_pages_are_target = false;
};

class HiCacheFactParser {
  public:
    bool is_hicache_event(const TraceEvent & event) const;
    HiCacheFact parse(size_t node_id, const TraceEvent & event) const;

  private:
    std::string infer_role(const TraceEvent & event) const;
    std::vector<std::string> parse_page_identity(const TraceEvent & event) const;
    std::vector<std::string> parse_page_arg(const TraceEvent & event, const std::string & key) const;
    std::unordered_map<uint64_t, std::vector<std::string>> parse_page_arg_by_page_size(const TraceEvent & event, const std::string & prefix) const;
    void parse_prefetch_progress(const TraceEvent & event, HiCacheFact & fact) const;
    bool role_requires_page_identity(const std::string & role, const TraceEvent & event) const;
};

} // namespace TraceGraph
