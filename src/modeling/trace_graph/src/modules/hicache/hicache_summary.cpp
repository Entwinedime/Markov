/**
 * @file
 * @brief HiCache 状态模型 summary JSON 序列化。
 */
#include "trace_graph/modules/hicache/hicache_summary.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <ranges>

namespace TraceGraph {

namespace {

using Json = nlohmann::json;

/** @brief 将任意 range 投影成 JSON array，保持 summary 序列化入口只描述字段结构。 */
Json json_array_from(std::ranges::input_range auto && range, auto projector) {
    Json rows = Json::array();
    std::ranges::for_each(range, [&](const auto & item) { rows.push_back(std::invoke(projector, item)); });
    return rows;
}

/** @brief 将 transition row 序列化为 JSON，并按配置决定是否包含 digest。 */
Json transition_to_json(const HiCacheStateTransition & transition, bool emit_state_digests) {
    Json row = {
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
    if (emit_state_digests) {
        row["before_state_digest"] = transition.before_state_digest;
        row["after_state_digest"] = transition.after_state_digest;
    }
    return row;
}

/** @brief 序列化 target config，和 final state 输出分离。 */
Json target_config_to_json(const HiCacheConfig & config) {
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
        {                "emit_state_digests",                config.emit_state_digests },
    };
}

/** @brief 序列化 final state 及其计数，避免 to_json 主流程混入重复 size 字段。 */
Json final_state_to_json(const HiCacheSummary & summary) {
    Json final_state = {
        {         "l1_resident_pages",         summary.l1_resident_pages },
        {         "l2_resident_pages",         summary.l2_resident_pages },
        {         "l3_resident_pages",         summary.l3_resident_pages },
        {               "dirty_pages",               summary.dirty_pages },
        {            "backuped_pages",            summary.backuped_pages },
        {             "evicted_pages",             summary.evicted_pages },
        {              "locked_pages",              summary.locked_pages },
        {   "pending_writeback_pages",   summary.pending_writeback_pages },
        {    "prefetch_planned_pages",    summary.prefetch_planned_pages },
        {      "prefetch_ready_pages",      summary.prefetch_ready_pages },
        {       "prefetch_late_pages",       summary.prefetch_late_pages },
        { "prefetch_suppressed_pages", summary.prefetch_suppressed_pages },
        {           "page_hit_counts",           summary.page_hit_counts },
    };
    final_state["counts"] = {
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
    return final_state;
}

} // namespace

/**
 * @brief 序列化 HiCache summary。
 *
 * summary 描述 HiCache 状态验证结果，不参与默认 E2E prediction；final_state 和
 * transition_trace 是消费者审查 target-state 状态模型的主要入口。
 */
std::string HiCacheSummary::to_json() const {
    Json root;
    root["status"] = status;
    root["input_hicache_events"] = input_hicache_events;
    root["processed_hicache_events"] = processed_hicache_events;
    root["state_transition_count"] = state_transition_count;
    root["dag_mutations"] = dag_mutations;
    root["dirty_eviction_events"] = dirty_eviction_events;
    root["skipped_non_invariant_events"] = skipped_non_invariant_events;
    root["target_config"] = target_config_to_json(target_config);
    root["events_by_role"] = events_by_role;
    root["processed_events_by_role"] = processed_events_by_role;
    root["transitions_by_kind"] = transitions_by_kind;
    root["missing_invariant_facts"] = missing_invariant_facts;
    root["non_invariant_fact_usage_by_role"] = non_invariant_fact_usage_by_role;
    root["non_invariant_fact_usage"] = json_array_from(non_invariant_fact_usage_by_role, [](const auto & item) {
        const auto & [role, count] = item;
        return Json{
            {  "role",  role },
            { "count", count }
        };
    });
    root["transition_trace"] = json_array_from(transition_trace, [&](const HiCacheStateTransition & transition) {
        return transition_to_json(transition, target_config.emit_state_digests);
    });
    root["final_state"] = final_state_to_json(*this);
    root["warnings"] = warnings;
    return root.dump();
}

} // namespace TraceGraph
