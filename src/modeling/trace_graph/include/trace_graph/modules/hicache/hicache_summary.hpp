#pragma once

#include "trace_graph/frontend/model_config.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief 单个 target-state mutation 的审计记录。
 *
 * transition trace 只描述模型内部 state mutation，不代表 DAG 已经被修改。
 * digest 字段由配置控制，便于在需要时定位状态分叉。
 */
struct HiCacheStateTransition {
    std::string transition_id;
    std::string kind;
    std::string role;
    std::string request_id;
    std::string operation_id;
    std::string event_name;
    std::string cache_scope;
    uint64_t ts = 0;
    size_t source_event_index = 0;
    std::string tier;
    std::vector<std::string> pages;
    std::string before_state_digest;
    std::string after_state_digest;
};

/**
 * @brief HiCache 状态模型的输出摘要。
 *
 * summary 汇总输入事件、被处理的 invariant fact、缺失字段、transition trace 和最终
 * page state。当前模块只维护 state，不修改 DAG；dag_mutations 保持为显式输出字段，
 * 用于防止后续误把状态建模与 DAG rewrite 混在一起。
 */
struct HiCacheSummary {
    std::string status = "state_model";
    HiCacheConfig target_config;
    uint64_t input_hicache_events = 0;
    uint64_t processed_hicache_events = 0;
    uint64_t state_transition_count = 0;
    uint64_t dag_mutations = 0;
    uint64_t dirty_eviction_events = 0;
    uint64_t skipped_non_invariant_events = 0;
    std::map<std::string, uint64_t> events_by_role;
    std::map<std::string, uint64_t> processed_events_by_role;
    std::map<std::string, uint64_t> transitions_by_kind;
    std::map<std::string, uint64_t> missing_invariant_facts;
    std::map<std::string, uint64_t> non_invariant_fact_usage_by_role;
    std::vector<std::string> l1_resident_pages;
    std::vector<std::string> l2_resident_pages;
    std::vector<std::string> l3_resident_pages;
    std::vector<std::string> dirty_pages;
    std::vector<std::string> backuped_pages;
    std::vector<std::string> evicted_pages;
    std::vector<std::string> locked_pages;
    std::vector<std::string> pending_writeback_pages;
    std::vector<std::string> prefetch_planned_pages;
    std::vector<std::string> prefetch_ready_pages;
    std::vector<std::string> prefetch_late_pages;
    std::vector<std::string> prefetch_suppressed_pages;
    std::map<std::string, uint64_t> page_hit_counts;
    std::vector<HiCacheStateTransition> transition_trace;
    std::vector<std::string> warnings;

    /** @brief 序列化为模块 summary JSON。 */
    std::string to_json() const;
};

} // namespace TraceGraph
