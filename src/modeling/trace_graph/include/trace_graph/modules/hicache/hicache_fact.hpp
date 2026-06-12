#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

struct TraceEvent;

struct HiCacheToken {
    std::vector<uint32_t> words;
};

using HiCacheTokenPath = std::vector<HiCacheToken>;

struct HiCacheTokenSpan {
    std::string path_id;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t token_count = 0;
    std::string hash_algo;
    bool valid = false;
};

struct HiCacheFact {
    size_t source_node_id = 0;
    size_t source_event_index = 0;
    uint64_t ts = 0;
    uint64_t dur = 0;
    std::string event_name;
    std::string target_id;
    std::string fact_class;
    std::string fact_granularity;
    std::string role;
    std::string phase;
    std::string request_id;
    std::string operation_id;
    std::string cache_scope;
    std::string check_kind;
    std::string lifecycle_kind;
    std::string admission_kind;
    uint64_t seq_no = 0;
    uint64_t source_page_size = 0;
    uint64_t token_count = 0;
    uint64_t max_new_tokens = 0;
    uint64_t truncation_align_size = 0;
    int64_t priority = 0;
    bool has_chunked_req = false;
    bool ignore_eos = false;
    bool model_input = false;
    bool dag_input = false;
    bool is_start = false;
    bool is_end = false;

    HiCacheTokenSpan full_path_span;

    HiCacheTokenPath full_path_tokens;
    std::unordered_map<std::string, std::vector<std::string>> diagnostic_state_pages;
};

class HiCacheFactParser {
  public:
    bool is_hicache_event(const TraceEvent & event) const;
    void observe_token_dictionaries(const TraceEvent & event);
    HiCacheFact parse(size_t node_id, const TraceEvent & event) const;

  private:
    std::unordered_map<std::string, HiCacheTokenPath> token_paths_;

    HiCacheTokenSpan parse_span(const TraceEvent & event, const std::string & key) const;
    HiCacheTokenPath resolve_span(const HiCacheTokenSpan & span) const;
    void observe_dictionary_value(const std::string & raw);
};

} // namespace TraceGraph
