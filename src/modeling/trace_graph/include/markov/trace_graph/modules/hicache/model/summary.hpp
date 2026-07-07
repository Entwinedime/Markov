/**
 * @file
 * @brief HiCache 状态模型的 Debug/validation 结构化结果。
 *
 * 本文件只定义模型执行后供 Debug/validation 消费的结构化数据，不包含 JSON
 * 序列化、文件输出或 validation oracle 对比。Release 业务 backend 执行 state replay
 * 后不暴露也不长期保存这些结构；diagnostics 层可以在 Debug 构建中消费这些结构生成
 * summary/debug 输出，但状态机核心不反向依赖 diagnostics。
 */
#pragma once

#ifdef DEBUG
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/policy.hpp"
#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"
#include "markov/trace_graph/modules/hicache/runtime/capacity_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/ref_ledger.hpp"
#include "markov/trace_graph/modules/hicache/runtime/state_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_control_clock.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#endif

namespace markov::trace_graph::modules::hicache::model {

#ifdef DEBUG
using radix::HiCacheNodeSplitRecord;
using runtime::HiCacheCapacityAuditIssue;
using runtime::HiCacheCapacityMutation;
using runtime::HiCacheCapacityVictimChoice;
using runtime::HiCacheControlBoundary;
using runtime::HiCacheDerivedStateSnapshot;
using runtime::HiCacheOperationLifecycleTransition;
using runtime::HiCacheRefAuditIssue;
using runtime::HiCacheRefMutation;

/**
 * @brief 单个 target-derived state mutation 的结构化记录。
 *
 * 记录描述模型内部状态如何随某个 fact 变化，`pages` 使用模型 canonical page id；
 * `source_event_index` 指向输入 trace event 在归一化序列中的位置。digest 字段仅在
 * `HiCacheConfig::emit_state_digests` 打开时填充，不参与状态机决策。
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

using HiCacheTransitionBuffer = std::vector<HiCacheStateTransition>;
#else
struct HiCacheTransitionBuffer {};
#endif

/**
 * @brief HiCache state model 的 Debug/validation 执行结果。
 *
 * 该结构是 state replay 与 diagnostics/validation 层之间的唯一汇总边界：Debug 构建
 * 会填充 fact 计数、policy 决策、async lifecycle、capacity/ref 审计和 final
 * derived state；JSON 字段名、pretty print 和 oracle 对比逻辑均在外层完成。Release
 * 构建不把这些字段作为业务输出持有。
 *
 * 关键不变量：
 * - `dag_mutations` 表示 HiCache module 在显式开启 DAG patch 后写入的 DAG mutation 数；
 * - `*_trace` 中的 page/node 单位均为模型 canonical page/node，不是原始 token；
 * - `final_state_derivation_mode` 说明 final state 使用的 derived-state 投影模式；
 * - `storage_directory_inclusive_state` 是包含 storage directory 投影的补充视图，
 *   不替代 `final_state` 中的 materialized-only 页面集合。
 */
struct HiCacheSummary {
#ifdef DEBUG
    std::string status = "state_model";
    frontend::HiCacheConfig target_config;
    HiCacheResolvedPolicyState resolved_policy;
    uint64_t input_hicache_events = 0;
    uint64_t processed_hicache_events = 0;
    uint64_t state_transition_count = 0;
    uint64_t dag_mutations = 0;
    std::string dag_patch_model;
    std::string dag_patch_workload_band;
    uint64_t dag_patch_source_e2e_ns = 0;
    uint64_t dag_patch_source_critical_interval_ns = 0;
    uint64_t dag_patch_target_critical_interval_ns = 0;
    uint64_t dag_patch_interval_node_count = 0;
    double dag_patch_interval_scale = 1.0;
    uint64_t dirty_eviction_events = 0;
    uint64_t active_ref_owner_count = 0;
    uint64_t radix_split_count = 0;
    uint64_t control_boundary_count = 0;
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
    std::vector<HiCacheControlBoundary> control_boundary_trace;
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
#endif
};

} // namespace markov::trace_graph::modules::hicache::model
