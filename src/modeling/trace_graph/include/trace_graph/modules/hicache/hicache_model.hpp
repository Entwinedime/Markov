#pragma once

#include "trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace TraceGraph {

class DagGraph;

// HiCache 状态模型的输出摘要。
// 当前维护 page resident/dirty/backuped 状态，但不修改 DAG。
struct HiCacheSummary {
    std::string status = "state_model";
    uint64_t input_hicache_events = 0;
    uint64_t processed_hicache_events = 0;
    uint64_t state_transition_count = 0;
    uint64_t dag_mutations = 0;
    uint64_t missing_page_identity_events = 0;
    uint64_t dirty_eviction_events = 0;
    std::map<std::string, uint64_t> events_by_role;
    std::map<std::string, uint64_t> processed_events_by_role;
    std::map<std::string, uint64_t> transitions_by_kind;
    std::vector<std::string> l1_resident_pages;
    std::vector<std::string> l2_resident_pages;
    std::vector<std::string> l3_resident_pages;
    std::vector<std::string> dirty_pages;
    std::vector<std::string> backuped_pages;
    std::vector<std::string> evicted_pages;
    std::vector<std::string> prefetch_planned_pages;
    std::vector<std::string> prefetch_ready_pages;
    std::vector<std::string> warnings;

    std::string to_json() const;
};

// HiCache 功能模型入口。
// 当前阶段只做状态维护和验证摘要，DAG patch 后续在同一模块内继续扩展。
HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config);

} // namespace TraceGraph
