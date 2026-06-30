/**
 * @file
 * @brief HiCache summary JSON 序列化。
 *
 * 本文件属于 diagnostics 层：只把 model::HiCacheSummary 转成稳定 JSON，不读取
 * profiling trace、不回写 state，也不参与 validation oracle 判定。
 */
#include "markov/trace_graph/modules/hicache/diagnostics/summary.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <ranges>

namespace markov::trace_graph::modules::hicache::diagnostics {

using model::HiCacheCapacityAuditIssue;
using model::HiCacheCapacityMutation;
using model::HiCacheCapacityVictimChoice;
using model::HiCacheControlCheckpoint;
using model::HiCacheNodeSplitRecord;
using model::HiCacheOperationLifecycleTransition;
using model::HiCacheRefAuditIssue;
using model::HiCacheRefMutation;
using model::HiCacheStateTransition;
using model::HiCacheSummary;
using radix::HiCacheNodeResidency;
using radix::HiCacheNodeSplitProjection;
using runtime::hicache_derived_state_mode_name;
using runtime::hicache_sorted_vector;
using runtime::HiCacheDerivedStateSnapshot;
using runtime::HiCacheOperationKind;
using runtime::HiCacheOperationState;

namespace summary_detail {

using Json = nlohmann::json;

Json array_from(std::ranges::input_range auto && range, auto projector) {
    Json output = Json::array();
    std::ranges::for_each(range, [&](const auto & value) { output.push_back(std::invoke(projector, value)); });
    return output;
}

Json target_config_json(const frontend::HiCacheConfig & config) {
    return {
        {                         "page_size",                         config.page_size },
        {                 "l1_capacity_pages",                 config.l1_capacity_pages },
        {                 "l2_capacity_pages",                 config.l2_capacity_pages },
        {                      "write_policy",                      config.write_policy },
        {           "write_through_threshold",           config.write_through_threshold },
        {                   "prefetch_policy",                   config.prefetch_policy },
        {          "prefetch_threshold_pages",          config.prefetch_threshold_pages },
        {     "prefetch_capacity_limit_pages",     config.prefetch_capacity_limit_pages },
        {       "prefetch_timeout_configured",       config.prefetch_timeout_configured },
        {         "prefetch_timeout_base_sec",         config.prefetch_timeout_base_sec },
        { "prefetch_timeout_per_ki_token_sec", config.prefetch_timeout_per_ki_token_sec },
        {          "prefetch_timeout_max_sec",          config.prefetch_timeout_max_sec },
        {        "device_allocator_need_sort",        config.device_allocator_need_sort },
        {                "emit_state_digests",                config.emit_state_digests },
    };
}

Json resolved_policy_json(const HiCacheResolvedPolicyState & policy) {
    return {
        {                         "page_size",                         policy.page_size },
        {                  "page_size_source",                  policy.page_size_source },
        {                 "l1_capacity_pages",                 policy.l1_capacity_pages },
        {                "l1_capacity_source",                policy.l1_capacity_source },
        {                 "l2_capacity_pages",                 policy.l2_capacity_pages },
        {                "l2_capacity_source",                policy.l2_capacity_source },
        {                      "write_policy",                      policy.write_policy },
        {               "write_policy_source",               policy.write_policy_source },
        {           "write_through_threshold",           policy.write_through_threshold },
        {    "write_through_threshold_source",    policy.write_through_threshold_source },
        {               "write_count_enabled",               policy.write_count_enabled },
        {                "write_back_enabled",                policy.write_back_enabled },
        {                   "prefetch_policy",                   policy.prefetch_policy },
        {            "prefetch_policy_source",            policy.prefetch_policy_source },
        {         "prefetch_threshold_tokens",         policy.prefetch_threshold_tokens },
        {          "prefetch_threshold_pages",          policy.prefetch_threshold_pages },
        {         "prefetch_threshold_source",         policy.prefetch_threshold_source },
        {     "prefetch_capacity_limit_pages",     policy.prefetch_capacity_limit_pages },
        {    "prefetch_capacity_limit_source",    policy.prefetch_capacity_limit_source },
        {          "host_cleanup_budget_rule",          policy.host_cleanup_budget_rule },
        {        "host_cleanup_budget_source",        policy.host_cleanup_budget_source },
        {      "extend_allocation_batch_size",      policy.extend_allocation_batch_size },
        {    "extend_allocation_batch_source",    policy.extend_allocation_batch_source },
        {            "extend_allocation_rule",            policy.extend_allocation_rule },
        {        "device_allocator_need_sort",        policy.device_allocator_need_sort },
        { "device_allocator_need_sort_source", policy.device_allocator_need_sort_source },
        {                "storage_hit_policy",                policy.storage_hit_policy },
        {         "storage_hit_policy_source",         policy.storage_hit_policy_source },
        {       "prefetch_timeout_configured",       policy.prefetch_timeout_configured },
        {         "prefetch_timeout_base_sec",         policy.prefetch_timeout_base_sec },
        { "prefetch_timeout_per_ki_token_sec", policy.prefetch_timeout_per_ki_token_sec },
        {          "prefetch_timeout_max_sec",          policy.prefetch_timeout_max_sec },
        {           "prefetch_timeout_source",           policy.prefetch_timeout_source },
        {             "prefetch_timeout_rule",             policy.prefetch_timeout_rule },
        {          "prefetch_rate_limit_rule",          policy.prefetch_rate_limit_rule },
        {                  "resolution_notes",                  policy.resolution_notes },
    };
}

std::string operation_kind_name(HiCacheOperationKind kind) {
    switch (kind) {
    case HiCacheOperationKind::Prefetch:
        return "prefetch";
    case HiCacheOperationKind::Writeback:
        return "writeback";
    case HiCacheOperationKind::Loadback:
        return "loadback";
    case HiCacheOperationKind::Storage:
        return "storage";
    }
    return "unknown";
}

std::string operation_state_name(HiCacheOperationState state) {
    switch (state) {
    case HiCacheOperationState::Created:
        return "created";
    case HiCacheOperationState::Queued:
        return "queued";
    case HiCacheOperationState::Ready:
        return "ready";
    case HiCacheOperationState::Completed:
        return "completed";
    case HiCacheOperationState::Committed:
        return "committed";
    case HiCacheOperationState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

Json transition_json(const HiCacheStateTransition & transition, bool emit_digest) {
    Json row{
        {      "transition_id",      transition.transition_id },
        {               "kind",               transition.kind },
        {               "role",               transition.role },
        {         "request_id",         transition.request_id },
        {       "operation_id",       transition.operation_id },
        {         "event_name",         transition.event_name },
        {        "cache_scope",        transition.cache_scope },
        {                 "ts",                 transition.ts },
        { "source_event_index", transition.source_event_index },
        {               "tier",               transition.tier },
        {              "pages",              transition.pages },
    };
    if (emit_digest) {
        row["before_state_digest"] = transition.before_state_digest;
        row["after_state_digest"] = transition.after_state_digest;
    }
    return row;
}

Json async_lifecycle_json(const HiCacheOperationLifecycleTransition & transition) {
    return Json{
        {     "operation_id",                     transition.operation_id },
        {             "kind",        operation_kind_name(transition.kind) },
        {       "from_state", operation_state_name(transition.from_state) },
        {         "to_state",   operation_state_name(transition.to_state) },
        { "transition_epoch",                 transition.transition_epoch },
        {    "transition_ts",                    transition.transition_ts },
        {      "cache_scope",                      transition.cache_scope },
        {      "request_key",                      transition.request_key },
        {           "reason",                           transition.reason },
    };
}

Json control_checkpoint_json(const HiCacheControlCheckpoint & checkpoint) {
    return Json{
        {    "scheduler_epoch",    checkpoint.scheduler_epoch },
        {   "checkpoint_epoch",   checkpoint.checkpoint_epoch },
        {        "cache_scope",        checkpoint.cache_scope },
        {        "request_key",        checkpoint.request_key },
        {               "kind",               checkpoint.kind },
        {             "source",             checkpoint.source },
        { "source_event_index", checkpoint.source_event_index },
        {                 "ts",                 checkpoint.ts },
        {           "terminal",           checkpoint.terminal },
    };
}

Json policy_decision_json(const HiCachePolicyDecisionRecord & decision) {
    return Json{
        {                   "decision_epoch",                   decision.decision_epoch },
        {                      "cache_scope",                      decision.cache_scope },
        {                      "request_key",                      decision.request_key },
        {                     "operation_id",                     decision.operation_id },
        {                             "role",                             decision.role },
        {                       "event_name",                       decision.event_name },
        {                      "policy_area",                      decision.policy_area },
        {                      "policy_name",                      decision.policy_name },
        {                         "decision",                         decision.decision },
        {                           "reason",                           decision.reason },
        {                         "accepted",                         decision.accepted },
        {                  "requested_pages",                  decision.requested_pages },
        {                 "requested_tokens",                 decision.requested_tokens },
        {                  "candidate_pages",                  decision.candidate_pages },
        {                        "hit_pages",                        decision.hit_pages },
        {                        "hit_count",                        decision.hit_count },
        {                       "batch_size",                       decision.batch_size },
        {                    "extend_tokens",                    decision.extend_tokens },
        {                  "allocated_pages",                  decision.allocated_pages },
        {           "active_requested_pages",           decision.active_requested_pages },
        {                   "capacity_pages",                   decision.capacity_pages },
        {                   "occupied_pages",                   decision.occupied_pages },
        {                   "reserved_pages",                   decision.reserved_pages },
        {             "allocator_free_pages",             decision.allocator_free_pages },
        {          "allocator_release_pages",          decision.allocator_release_pages },
        {        "allocator_available_pages",        decision.allocator_available_pages },
        { "allocator_available_before_pages", decision.allocator_available_before_pages },
        {         "allocator_consumed_pages",         decision.allocator_consumed_pages },
        {         "allocator_released_pages",         decision.allocator_released_pages },
        {        "lifecycle_duplicate_pages",        decision.lifecycle_duplicate_pages },
        {             "lifecycle_tail_pages",             decision.lifecycle_tail_pages },
        {                  "threshold_pages",                  decision.threshold_pages },
        {                      "limit_pages",                      decision.limit_pages },
        {                            "pages",                            decision.pages },
    };
}

Json residency_json(const HiCacheNodeResidency & residency) {
    return Json{
        {   "device_present",   residency.device_present },
        {     "device_dirty",     residency.device_dirty },
        {     "host_present",     residency.host_present },
        {     "host_visible",     residency.host_visible },
        {    "storage_known",    residency.storage_known },
        { "storage_readable", residency.storage_readable },
    };
}

Json split_projection_json(const HiCacheNodeSplitProjection & projection) {
    return Json{
        { "depth_page_begin", projection.depth_page_begin },
        {   "depth_page_end",   projection.depth_page_end },
        { "token_span_known", projection.token_span_known },
        {      "token_begin",      projection.token_begin },
        {        "token_end",        projection.token_end },
        {      "page_hashes",      projection.page_hashes },
        {     "storage_keys",     projection.storage_keys },
    };
}

Json split_record_json(const HiCacheNodeSplitRecord & split) {
    return Json{
        {            "cache_scope",                              split.cache_scope },
        {            "parent_node",                              split.parent_node },
        {            "prefix_node",                              split.prefix_node },
        {            "suffix_node",                              split.suffix_node },
        {            "split_pages",                              split.split_pages },
        {       "parent_child_key",                         split.parent_child_key },
        {       "suffix_child_key",                         split.suffix_child_key },
        {           "prefix_pages",                             split.prefix_pages },
        {           "suffix_pages",                             split.suffix_pages },
        {      "prefix_projection", split_projection_json(split.prefix_projection) },
        {      "suffix_projection", split_projection_json(split.suffix_projection) },
        {       "prefix_residency",         residency_json(split.prefix_residency) },
        {       "suffix_residency",         residency_json(split.suffix_residency) },
        {  "copied_lock_ref_total",                    split.copied_lock_ref_total },
        {  "copied_host_ref_total",                    split.copied_host_ref_total },
        { "copied_lock_ref_owners",                   split.copied_lock_ref_owners },
        { "copied_host_ref_owners",                   split.copied_host_ref_owners },
        {    "inherited_hit_count",                      split.inherited_hit_count },
    };
}

Json capacity_mutation_json(const HiCacheCapacityMutation & mutation) {
    return Json{
        {      "mutation_epoch",      mutation.mutation_epoch },
        {         "cache_scope",         mutation.cache_scope },
        {              "reason",              mutation.reason },
        { "reserved_host_pages", mutation.reserved_host_pages },
        {      "observed_nodes",      mutation.observed_nodes },
        { "device_leaf_entered", mutation.device_leaf_entered },
        {    "device_leaf_left",    mutation.device_leaf_left },
        {   "host_leaf_entered",   mutation.host_leaf_entered },
        {      "host_leaf_left",      mutation.host_leaf_left },
    };
}

Json capacity_victim_choice_json(const HiCacheCapacityVictimChoice & choice) {
    return Json{
        {     "selection_epoch",     choice.selection_epoch },
        {         "cache_scope",         choice.cache_scope },
        {                "tier",                choice.tier },
        {              "reason",              choice.reason },
        {            "selected",            choice.selected },
        {             "node_id",             choice.node_id },
        {          "page_count",          choice.page_count },
        {            "priority",            choice.priority },
        {   "last_access_order",   choice.last_access_order },
        {      "occupied_pages",      choice.occupied_pages },
        { "reserved_host_pages", choice.reserved_host_pages },
        {      "capacity_pages",      choice.capacity_pages },
        {     "requested_pages",     choice.requested_pages },
        {        "excess_pages",        choice.excess_pages },
        {               "pages",               choice.pages },
    };
}

Json capacity_audit_issue_json(const HiCacheCapacityAuditIssue & issue) {
    return Json{
        {   "cache_scope",   issue.cache_scope },
        {         "issue",         issue.issue },
        {          "tier",          issue.tier },
        {       "node_id",       issue.node_id },
        { "indexed_count", issue.indexed_count },
        {    "tree_count",    issue.tree_count },
        {         "pages",         issue.pages },
    };
}

Json ref_mutation_json(const HiCacheRefMutation & mutation) {
    return Json{
        { "mutation_epoch", mutation.mutation_epoch },
        {    "cache_scope",    mutation.cache_scope },
        {         "action",         mutation.action },
        {       "owner_id",       mutation.owner_id },
        {     "owner_kind",     mutation.owner_kind },
        {    "request_key",    mutation.request_key },
        {   "operation_id",   mutation.operation_id },
        {         "reason",         mutation.reason },
        {     "lock_nodes",     mutation.lock_nodes },
        {     "host_nodes",     mutation.host_nodes },
        {     "lock_pages",     mutation.lock_pages },
        {     "host_pages",     mutation.host_pages },
        { "lock_ref_delta", mutation.lock_ref_delta },
        { "host_ref_delta", mutation.host_ref_delta },
        {        "changed",        mutation.changed },
    };
}

Json ref_audit_issue_json(const HiCacheRefAuditIssue & issue) {
    return Json{
        {  "cache_scope",  issue.cache_scope },
        {        "issue",        issue.issue },
        {     "ref_kind",     issue.ref_kind },
        {     "owner_id",     issue.owner_id },
        {      "node_id",      issue.node_id },
        { "ledger_count", issue.ledger_count },
        {   "tree_count",   issue.tree_count },
        {        "pages",        issue.pages },
    };
}

Json final_state_json(const HiCacheSummary & summary) {
    Json state{
        {           "derivation_mode", summary.final_state_derivation_mode },
        {         "l1_resident_pages",           summary.l1_resident_pages },
        {         "l2_resident_pages",           summary.l2_resident_pages },
        {         "l3_resident_pages",           summary.l3_resident_pages },
        {               "dirty_pages",                 summary.dirty_pages },
        {            "backuped_pages",              summary.backuped_pages },
        {             "evicted_pages",               summary.evicted_pages },
        {              "locked_pages",                summary.locked_pages },
        {   "pending_writeback_pages",     summary.pending_writeback_pages },
        {    "prefetch_planned_pages",      summary.prefetch_planned_pages },
        {      "prefetch_ready_pages",        summary.prefetch_ready_pages },
        {       "prefetch_late_pages",         summary.prefetch_late_pages },
        { "prefetch_suppressed_pages",   summary.prefetch_suppressed_pages },
        {           "page_hit_counts",             summary.page_hit_counts },
    };
    state["counts"] = {
        {         "l1_resident_pages",         summary.l1_resident_pages.size() },
        {         "l2_resident_pages",         summary.l2_resident_pages.size() },
        {         "l3_resident_pages",         summary.l3_resident_pages.size() },
        {               "dirty_pages",               summary.dirty_pages.size() },
        {            "backuped_pages",            summary.backuped_pages.size() },
        {             "evicted_pages",             summary.evicted_pages.size() },
        {              "locked_pages",              summary.locked_pages.size() },
        {   "pending_writeback_pages",   summary.pending_writeback_pages.size() },
        {    "prefetch_planned_pages",    summary.prefetch_planned_pages.size() },
        {      "prefetch_ready_pages",      summary.prefetch_ready_pages.size() },
        {       "prefetch_late_pages",       summary.prefetch_late_pages.size() },
        { "prefetch_suppressed_pages", summary.prefetch_suppressed_pages.size() },
        {           "page_hit_counts",           summary.page_hit_counts.size() },
    };
    return state;
}

Json derived_snapshot_json(const HiCacheDerivedStateSnapshot & snapshot) {
    Json state{
        {           "derivation_mode",      hicache_derived_state_mode_name(snapshot.mode) },
        {         "l1_resident_pages",                  hicache_sorted_vector(snapshot.l1) },
        {         "l2_resident_pages",                  hicache_sorted_vector(snapshot.l2) },
        {         "l3_resident_pages",                  hicache_sorted_vector(snapshot.l3) },
        {               "dirty_pages",               hicache_sorted_vector(snapshot.dirty) },
        {            "backuped_pages",            hicache_sorted_vector(snapshot.backuped) },
        {             "evicted_pages",             hicache_sorted_vector(snapshot.evicted) },
        {              "locked_pages",              hicache_sorted_vector(snapshot.locked) },
        {   "pending_writeback_pages",   hicache_sorted_vector(snapshot.pending_writeback) },
        {    "prefetch_planned_pages",    hicache_sorted_vector(snapshot.prefetch_planned) },
        {      "prefetch_ready_pages",      hicache_sorted_vector(snapshot.prefetch_ready) },
        {       "prefetch_late_pages",       hicache_sorted_vector(snapshot.prefetch_late) },
        { "prefetch_suppressed_pages", hicache_sorted_vector(snapshot.prefetch_suppressed) },
        {           "page_hit_counts",                            snapshot.page_hit_counts },
    };
    state["counts"] = {
        {         "l1_resident_pages",                  snapshot.l1.size() },
        {         "l2_resident_pages",                  snapshot.l2.size() },
        {         "l3_resident_pages",                  snapshot.l3.size() },
        {               "dirty_pages",               snapshot.dirty.size() },
        {            "backuped_pages",            snapshot.backuped.size() },
        {             "evicted_pages",             snapshot.evicted.size() },
        {              "locked_pages",              snapshot.locked.size() },
        {   "pending_writeback_pages",   snapshot.pending_writeback.size() },
        {    "prefetch_planned_pages",    snapshot.prefetch_planned.size() },
        {      "prefetch_ready_pages",      snapshot.prefetch_ready.size() },
        {       "prefetch_late_pages",       snapshot.prefetch_late.size() },
        { "prefetch_suppressed_pages", snapshot.prefetch_suppressed.size() },
        {           "page_hit_counts",     snapshot.page_hit_counts.size() },
    };
    return state;
}

} // namespace summary_detail

using summary_detail::array_from;
using summary_detail::async_lifecycle_json;
using summary_detail::capacity_audit_issue_json;
using summary_detail::capacity_mutation_json;
using summary_detail::capacity_victim_choice_json;
using summary_detail::control_checkpoint_json;
using summary_detail::derived_snapshot_json;
using summary_detail::final_state_json;
using summary_detail::Json;
using summary_detail::policy_decision_json;
using summary_detail::ref_audit_issue_json;
using summary_detail::ref_mutation_json;
using summary_detail::resolved_policy_json;
using summary_detail::split_record_json;
using summary_detail::target_config_json;
using summary_detail::transition_json;

/**
 * @brief 将 HiCacheSummary 投影成稳定 diagnostics JSON。
 *
 * 该函数只序列化 state model 已经生成的事实、trace 和派生视图，不重新计算模型状态，
 * 也不把 diagnostics-only inclusive state 写回 final_state。
 */
std::string summary_json(const HiCacheSummary & summary) {
    Json root;
    root["status"] = summary.status;
    root["target_config"] = target_config_json(summary.target_config);
    root["resolved_policy"] = resolved_policy_json(summary.resolved_policy);
    root["input_hicache_events"] = summary.input_hicache_events;
    root["processed_hicache_events"] = summary.processed_hicache_events;
    root["state_transition_count"] = summary.state_transition_count;
    root["dag_mutations"] = summary.dag_mutations;
    root["dirty_eviction_events"] = summary.dirty_eviction_events;
    root["active_ref_owner_count"] = summary.active_ref_owner_count;
    root["radix_split_count"] = summary.radix_split_count;
    root["control_checkpoint_count"] = summary.control_checkpoint_count;
    root["async_lifecycle_transition_count"] = summary.async_lifecycle_transition_count;
    root["policy_decision_count"] = summary.policy_decision_count;
    root["storage_known_page_count"] = summary.storage_known_page_count;
    root["storage_readable_page_count"] = summary.storage_readable_page_count;
    root["storage_backend_readable_count"] = summary.storage_backend_readable_count;
    root["storage_materialized_page_count"] = summary.storage_materialized_page_count;
    root["capacity_mutation_count"] = summary.capacity_mutation_count;
    root["capacity_victim_choice_count"] = summary.capacity_victim_choice_count;
    root["capacity_audit_issue_count"] = summary.capacity_audit_issue_count;
    root["ref_mutation_count"] = summary.ref_mutation_count;
    root["ref_audit_issue_count"] = summary.ref_audit_issue_count;
    root["skipped_non_state_model_events"] = summary.skipped_non_state_model_events;
    root["final_state_derivation_mode"] = summary.final_state_derivation_mode;
    root["events_by_role"] = summary.events_by_role;
    root["processed_events_by_role"] = summary.processed_events_by_role;
    root["transitions_by_kind"] = summary.transitions_by_kind;
    root["missing_state_model_facts"] = summary.missing_state_model_facts;
    root["token_resolution_by_status"] = summary.token_resolution_by_status;
    root["token_path_diagnostics"] = summary.token_path_diagnostics;
    root["transition_trace"] =
        array_from(summary.transition_trace, [&](const auto & transition) { return transition_json(transition, summary.target_config.emit_state_digests); });
    root["radix_split_trace"] = array_from(summary.radix_split_trace, split_record_json);
    root["control_checkpoint_trace"] = array_from(summary.control_checkpoint_trace, control_checkpoint_json);
    root["async_lifecycle_trace"] = array_from(summary.async_lifecycle_trace, async_lifecycle_json);
    root["policy_decision_trace"] = array_from(summary.policy_decision_trace, policy_decision_json);
    root["capacity_mutation_trace"] = array_from(summary.capacity_mutation_trace, capacity_mutation_json);
    root["capacity_victim_choices"] = array_from(summary.capacity_victim_choices, capacity_victim_choice_json);
    root["capacity_audit_issues"] = array_from(summary.capacity_audit_issues, capacity_audit_issue_json);
    root["ref_mutation_trace"] = array_from(summary.ref_mutation_trace, ref_mutation_json);
    root["ref_audit_issues"] = array_from(summary.ref_audit_issues, ref_audit_issue_json);
    root["final_state"] = final_state_json(summary);
    root["storage_directory_inclusive_state"] = derived_snapshot_json(summary.storage_directory_inclusive_state);
    root["warnings"] = summary.warnings;
    return root.dump();
}

} // namespace markov::trace_graph::modules::hicache::diagnostics
