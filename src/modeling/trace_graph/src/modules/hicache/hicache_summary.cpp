#include "trace_graph/modules/hicache/hicache_summary.hpp"

#include <nlohmann/json.hpp>

namespace TraceGraph {

namespace {

using Json = nlohmann::json;

Json transition_to_json(const HiCacheStateTransition & transition, bool emit_state_digests) {
    Json row = {
        {"transition_id", transition.transition_id},
        {"kind", transition.kind},
        {"role", transition.role},
        {"request_id", transition.request_id},
        {"operation_id", transition.operation_id},
        {"event_name", transition.event_name},
        {"cache_scope", transition.cache_scope},
        {"ts", transition.ts},
        {"source_event_index", transition.source_event_index},
        {"tier", transition.tier},
        {"pages", transition.pages},
    };
    if (emit_state_digests) {
        row["before_state_digest"] = transition.before_state_digest;
        row["after_state_digest"] = transition.after_state_digest;
    }
    return row;
}

} // namespace

std::string HiCacheSummary::to_json() const {
    // summary 描述 HiCache 状态验证结果，不参与默认 E2E prediction。
    Json transition_rows = Json::array();
    for (const auto & transition : transition_trace) transition_rows.push_back(transition_to_json(transition, target_config.emit_state_digests));

    Json root;
    root["status"] = status;
    root["input_hicache_events"] = input_hicache_events;
    root["processed_hicache_events"] = processed_hicache_events;
    root["state_transition_count"] = state_transition_count;
    root["dag_mutations"] = dag_mutations;
    root["dirty_eviction_events"] = dirty_eviction_events;
    root["skipped_non_invariant_events"] = skipped_non_invariant_events;
    root["target_config"] = {
        {"page_size", target_config.page_size},
        {"l1_capacity_pages", target_config.l1_capacity_pages},
        {"l2_capacity_pages", target_config.l2_capacity_pages},
        {"write_policy", target_config.write_policy},
        {"write_through_threshold", target_config.write_through_threshold},
        {"prefetch_policy", target_config.prefetch_policy},
        {"prefetch_threshold_pages", target_config.prefetch_threshold_pages},
        {"prefetch_capacity_limit_pages", target_config.prefetch_capacity_limit_pages},
        {"prefetch_timeout_configured", target_config.prefetch_timeout_configured},
        {"prefetch_timeout_base_sec", target_config.prefetch_timeout_base_sec},
        {"prefetch_timeout_per_ki_token_sec", target_config.prefetch_timeout_per_ki_token_sec},
        {"prefetch_timeout_max_sec", target_config.prefetch_timeout_max_sec},
        {"emit_state_digests", target_config.emit_state_digests},
    };
    root["events_by_role"] = events_by_role;
    root["processed_events_by_role"] = processed_events_by_role;
    root["transitions_by_kind"] = transitions_by_kind;
    root["missing_invariant_facts"] = missing_invariant_facts;
    root["non_invariant_fact_usage_by_role"] = non_invariant_fact_usage_by_role;
    Json non_invariant_usage = Json::array();
    for (const auto & [role, count] : non_invariant_fact_usage_by_role) { non_invariant_usage.push_back({{"role", role}, {"count", count}}); }
    root["non_invariant_fact_usage"] = non_invariant_usage;
    root["transition_trace"] = transition_rows;
    root["final_state"] = {
        {"l1_resident_pages", l1_resident_pages},
        {"l2_resident_pages", l2_resident_pages},
        {"l3_resident_pages", l3_resident_pages},
        {"dirty_pages", dirty_pages},
        {"backuped_pages", backuped_pages},
        {"evicted_pages", evicted_pages},
        {"pending_writeback_pages", pending_writeback_pages},
        {"prefetch_planned_pages", prefetch_planned_pages},
        {"prefetch_ready_pages", prefetch_ready_pages},
        {"prefetch_late_pages", prefetch_late_pages},
        {"prefetch_suppressed_pages", prefetch_suppressed_pages},
        {"page_hit_counts", page_hit_counts},
        {"counts",
         {
             {"l1_resident_pages", l1_resident_pages.size()},
             {"l2_resident_pages", l2_resident_pages.size()},
             {"l3_resident_pages", l3_resident_pages.size()},
             {"dirty_pages", dirty_pages.size()},
             {"backuped_pages", backuped_pages.size()},
             {"evicted_pages", evicted_pages.size()},
             {"pending_writeback_pages", pending_writeback_pages.size()},
             {"prefetch_planned_pages", prefetch_planned_pages.size()},
             {"prefetch_ready_pages", prefetch_ready_pages.size()},
             {"prefetch_late_pages", prefetch_late_pages.size()},
             {"prefetch_suppressed_pages", prefetch_suppressed_pages.size()},
             {"page_hit_counts", page_hit_counts.size()},
         }},
    };
    root["final_state"]["locked_pages"] = locked_pages;
    root["final_state"]["counts"]["locked_pages"] = locked_pages.size();
    root["warnings"] = warnings;
    return root.dump();
}

} // namespace TraceGraph
