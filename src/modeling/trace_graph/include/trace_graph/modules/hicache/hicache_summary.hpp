#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/hicache/hicache_async_state.hpp"
#include "trace_graph/modules/hicache/hicache_capacity_index.hpp"
#include "trace_graph/modules/hicache/hicache_policy.hpp"
#include "trace_graph/modules/hicache/hicache_ref_ledger.hpp"
#include "trace_graph/modules/hicache/hicache_state_index.hpp"
#include "trace_graph/modules/hicache/hicache_target_control_clock.hpp"
#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief 单个 target-state mutation 的审计记录。
 *
 * transition trace 描述模型内部状态变化，不代表 DAG 已被 patch。
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
 * @brief HiCache 状态模型 summary。
 *
 * 当前 HiCacheModule 只维护 state model；`dag_mutations` 保持显式 0，避免把 state
 * alignment 与 DAG patch 混为一条验收线。
 */
struct HiCacheSummary {
    std::string status = "state_model";
    HiCacheConfig target_config;
    HiCacheResolvedPolicyState resolved_policy;
    uint64_t input_hicache_events = 0;
    uint64_t processed_hicache_events = 0;
    uint64_t state_transition_count = 0;
    uint64_t dag_mutations = 0;
    uint64_t dirty_eviction_events = 0;
    uint64_t active_ref_owner_count = 0;
    uint64_t radix_split_count = 0;
    uint64_t control_checkpoint_count = 0;
    uint64_t async_lifecycle_transition_count = 0;
    uint64_t policy_decision_count = 0;
    uint64_t storage_known_page_count = 0;
    uint64_t storage_readable_page_count = 0;
    uint64_t storage_backend_readable_count = 0;
    uint64_t storage_materialized_page_count = 0;
    uint64_t capacity_mutation_count = 0;
    uint64_t capacity_victim_choice_count = 0;
    uint64_t capacity_audit_issue_count = 0;
    uint64_t ref_mutation_count = 0;
    uint64_t ref_audit_issue_count = 0;
    uint64_t skipped_non_state_model_events = 0;
    std::string final_state_derivation_mode;
    std::map<std::string, uint64_t> events_by_role;
    std::map<std::string, uint64_t> processed_events_by_role;
    std::map<std::string, uint64_t> transitions_by_kind;
    std::map<std::string, uint64_t> missing_state_model_facts;
    std::map<std::string, uint64_t> token_resolution_by_status;
    std::map<std::string, uint64_t> token_path_diagnostics;
    std::vector<HiCacheNodeSplitRecord> radix_split_trace;
    std::vector<HiCacheControlCheckpoint> control_checkpoint_trace;
    std::vector<HiCacheOperationLifecycleTransition> async_lifecycle_trace;
    std::vector<HiCachePolicyDecisionRecord> policy_decision_trace;
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
    HiCacheDerivedStateSnapshot storage_directory_inclusive_state;
    std::vector<HiCacheCapacityMutation> capacity_mutation_trace;
    std::vector<HiCacheCapacityVictimChoice> capacity_victim_choices;
    std::vector<HiCacheCapacityAuditIssue> capacity_audit_issues;
    std::vector<HiCacheRefMutation> ref_mutation_trace;
    std::vector<HiCacheRefAuditIssue> ref_audit_issues;
    std::vector<HiCacheStateTransition> transition_trace;
    std::vector<std::string> warnings;

    [[nodiscard]] std::string to_json() const;
};

} // namespace TraceGraph
