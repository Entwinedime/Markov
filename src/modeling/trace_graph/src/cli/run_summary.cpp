/**
 * @file
 * @brief Serializes compact graph, simulation, and module run results.
 */
#include "run_summary.hpp"

#include "file_output.hpp"

#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

#include <nlohmann/json.hpp>

#include <ranges>

namespace markov::trace_graph::cli {

namespace {

using Json = nlohmann::json;

Json effect_decision_result(const modules::hicache::HiCacheModule & module) {
    const auto & ledger = module.effect_decisions();
    Json result;
    result["status"] = ledger.status;
    result["decision_count"] = ledger.decisions.size();
    result["patchable_count"] = ledger.patchable_count();
    result["not_patchable_count"] = ledger.not_patchable_count();
    result["deferred_count"] = ledger.deferred_count();
    result["unresolved_count"] = ledger.unresolved_count();
    result["schedule_sensitive_count"] = ledger.schedule_sensitive_count();
    result["decision_coverage"] = ledger.decision_coverage;
    result["prefill_effect_status"] = ledger.prefill_effect_status;
    result["counts_by_effect_type"] = ledger.counts_by_effect_type();
    result["counts_by_target_effect_state"] = ledger.counts_by_target_effect_state();
    result["counts_by_schedule_sensitivity"] = ledger.counts_by_schedule_sensitivity();
    result["counts_by_source_carrier_state"] = ledger.counts_by_source_carrier_state();
    result["missing_facts"] = ledger.missing_facts;
    result["not_patchable_reasons"] = ledger.not_patchable_reasons;
    result["byte_projection_available"] = ledger.byte_projection_available;
    result["kv_bytes_per_page"] = ledger.kv_bytes_per_page;
    result["l2_capacity_pages"] = ledger.l2_capacity_pages;
    result["l2_capacity_bytes"] = ledger.l2_capacity_bytes;
    result["byte_projection_source"] = ledger.byte_projection_source;
    return result;
}

Json io_resource_result(const modules::hicache::patch::HiCacheIoResourcePlan & resources) {
    Json result{
        {                 "status",                                                                    resources.status },
        { "byte_projection_source", resources.byte_projection_available ? "target_config.kv_bytes_per_page" : "missing" },
        {      "kv_bytes_per_page",                                                         resources.kv_bytes_per_page },
        {         "decision_count",                                                              resources.costs.size() },
        {       "cost_ready_count",                                                             resources.ready_count() },
        {  "lane_dependency_count",                                                  resources.lane_dependencies.size() },
        {  "counts_by_cost_status",                                                          resources.counts_by_status },
        {         "blocker_counts",                                                            resources.blocker_counts },
    };
#ifdef DEBUG
    const auto & oracle = resources.oracle_cost_replay;
    if (oracle.status != "disabled") {
        result["oracle_cost_replay"] = Json{
            {                       "status",                       oracle.status },
            {          "required_cost_count",          oracle.required_cost_count },
            {          "supplied_cost_count",          oracle.supplied_cost_count },
            {           "applied_cost_count",           oracle.applied_cost_count },
            {            "oracle_service_us",            oracle.oracle_service_us },
            {           "applied_service_us",           oracle.applied_service_us },
            {            "oracle_control_us",            oracle.oracle_control_us },
            { "applied_primitive_control_us", oracle.applied_primitive_control_us },
            {      "outcome_only_control_us",      oracle.outcome_only_control_us },
            {         "observed_blocking_us",         oracle.observed_blocking_us },
            {        "effect_identity_exact",        oracle.effect_identity_exact },
            {        "operation_shape_exact",        oracle.operation_shape_exact },
            {          "target_e2e_consumed",          oracle.target_e2e_consumed },
        };
    }
#endif
    return result;
}

Json dag_patch_result(const modules::hicache::HiCacheDagPatchModule & module) {
    const auto & result = module.result();
    const auto duration_update_count = static_cast<size_t>(
        std::ranges::count_if(result.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::SetNodeDuration; }));
    const auto e2e_eligibility_update_count = static_cast<size_t>(
        std::ranges::count_if(result.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::SetNodeE2eEligibility; }));
    const bool topology_valid = result.shadow_rewrite.topology_valid && result.applied_validation.topology_exact;
    const bool validation_ready = result.source_attribution.status == "ready" && result.shadow_rewrite.status == "ready"
                                  && result.boundary_validation.status == "ready" && result.applied_validation.status == "ready" && topology_valid;
    Json summary{
        {                       "status",result.status                                         },
        {        "prefill_effect_status",                 result.prefill_effect_status },
        {                    "component",                     result.journal.component },
        {               "mutation_count",                result.journal.records.size() },
        {        "duration_update_count",                        duration_update_count },
        { "e2e_eligibility_update_count",                 e2e_eligibility_update_count },
        {          "active_nodes_before",           result.journal.active_nodes_before },
        {           "active_nodes_after",            result.journal.active_nodes_after },
        {          "active_edges_before",           result.journal.active_edges_before },
        {           "active_edges_after",            result.journal.active_edges_after },
        {                 "io_resources",      io_resource_result(result.io_resources) },
        {               "topology_valid",                               topology_valid },
        {               "blocker_counts",                        result.apply_blockers },
        {       "rewrite_counts_by_kind", result.shadow_rewrite.counts_by_rewrite_kind },
        {                   "validation",
         Json{
         { "status", validation_ready ? "ready" : "not_ready" },
         { "decision_count", result.shadow_rewrite.decisions.size() },
         { "attributed_count", result.source_attribution.attributed_count() },
         { "rewrite_ready_count", result.shadow_rewrite.ready_count() },
         { "boundary_ready_count", result.boundary_validation.ready_count() },
         { "applied_ready_count", result.applied_validation.ready_count() },
         { "plan_journal_exact", result.applied_validation.plan_journal_exact },
         { "prospective_materialization_exact", result.applied_validation.prospective_materialization_exact },
         { "family_dependencies_exact", result.applied_validation.family_dependencies_exact },
         { "lane_dependencies_exact", result.applied_validation.lane_dependencies_exact },
         }                                                                            },
    };
#ifdef DEBUG
    if (result.causal_timing_audit.status != "disabled_without_oracle_cost") {
        Json causal_effects = Json::array();
        for (const auto & effect : result.causal_timing_audit.effects) {
            causal_effects.push_back(Json{
                {                              "effect_id",                              effect.effect_id },
                {                            "effect_type",                            effect.effect_type },
                {                           "rewrite_kind",                           effect.rewrite_kind },
                {                       "causal_path_kind",                       effect.causal_path_kind },
                {                                 "status",                                 effect.status },
                {                 "target_cost_node_count",                 effect.target_cost_node_count },
                {                "target_cost_duration_us",                effect.target_cost_duration_us },
                {             "completion_join_node_count",             effect.completion_join_node_count },
                {                    "consumer_node_count",                    effect.consumer_node_count },
                {       "cost_node_completion_response_us",       effect.cost_node_completion_response_us },
                {      "completion_join_start_response_us",      effect.completion_join_start_response_us },
                {             "consumer_start_response_us",             effect.consumer_start_response_us },
                {     "source_completion_wait_duration_us",     effect.source_completion_wait_duration_us },
                { "source_completion_wait_gap_duration_us", effect.source_completion_wait_gap_duration_us },
                {    "source_residual_unknown_duration_us",    effect.source_residual_unknown_duration_us },
                {               "foreground_path_expected",               effect.foreground_path_expected },
                {               "completion_join_required",               effect.completion_join_required },
                {       "source_readiness_topology_reused",       effect.source_readiness_topology_reused },
                {        "source_completion_wait_blocking",        effect.source_completion_wait_blocking },
            });
        }
        summary["causal_timing_audit"] = Json{
            {                            "status",                            result.causal_timing_audit.status },
            {            "target_cost_node_count",            result.causal_timing_audit.target_cost_node_count },
            {           "target_cost_duration_us",           result.causal_timing_audit.target_cost_duration_us },
            {          "full_with_target_cost_us",          result.causal_timing_audit.full_with_target_cost_us },
            {       "full_without_target_cost_us",       result.causal_timing_audit.full_without_target_cost_us },
            {      "full_target_cost_response_us",      result.causal_timing_audit.full_target_cost_response_us },
            {       "control_with_target_cost_us",       result.causal_timing_audit.control_with_target_cost_us },
            {    "control_without_target_cost_us",    result.causal_timing_audit.control_without_target_cost_us },
            {   "control_target_cost_response_us",   result.causal_timing_audit.control_target_cost_response_us },
            { "local_cost_sensitive_effect_count", result.causal_timing_audit.local_cost_sensitive_effect_count },
            {    "local_cost_hidden_effect_count",    result.causal_timing_audit.local_cost_hidden_effect_count },
            {                           "effects",                                    std::move(causal_effects) },
            {                    "restored_exact",                    result.causal_timing_audit.restored_exact },
        };
    }
#endif
    return summary;
}

Json module_results(const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    Json results = Json::object();
    for (const auto & module : modules) {
        if (const auto * node_scale = dynamic_cast<const modules::node_scale::NodeScaleModule *>(module.get())) {
            results["node_scale"] = Json{
                { "scaled_nodes", node_scale->scaled_nodes() }
            };
        }
        else if (const auto * hicache = dynamic_cast<const modules::hicache::HiCacheModule *>(module.get())) {
            results["hicache"] = Json{
                { "effect_decisions", effect_decision_result(*hicache) }
            };
        }
        else if (const auto * patch = dynamic_cast<const modules::hicache::HiCacheDagPatchModule *>(module.get())) {
            results["hicache_dag_patch"] = dag_patch_result(*patch);
        }
    }
    return results;
}

Json run_summary(const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    const auto stats = graph.summary_stats();
    Json root;
    root["parsed_record_count"] = graph.parsed_record_count();
    root["simulated_e2e_us"] = graph.e2e_time();
    root["simulated_control_e2e_us"] = graph.control_e2e_time();
    root["control_exclusion_interval_count"] = graph.control_exclusion_intervals().size();
    size_t blackbox_exclusion_count = 0;
    size_t snapshot_exclusion_count = 0;
    for (const auto & interval : graph.control_exclusion_intervals()) {
        if (interval.kind == core::DagControlExclusionKind::PrefillDecode) blackbox_exclusion_count++;
        else if (interval.kind == core::DagControlExclusionKind::ProfilingSnapshot) snapshot_exclusion_count++;
    }
    root["control_blackbox_exclusion_interval_count"] = blackbox_exclusion_count;
    root["control_snapshot_exclusion_interval_count"] = snapshot_exclusion_count;
#ifdef DEBUG
    root["real_e2e_us"] = graph.real_e2e_time();
#endif
    root["node_count"] = stats.active_node_count;
    root["trace_node_count"] = stats.active_trace_node_count;
    root["synthetic_node_count"] = stats.active_synthetic_node_count;
    root["edge_count"] = stats.active_edge_count;
    root["stored_node_count"] = graph.node_count();
    root["stored_edge_count"] = graph.edge_count();
    root["edge_counts_by_kind"] = stats.edge_counts_by_kind;
    root["module_results"] = module_results(modules);
    return root;
}

} // namespace

void write_run_summary(const std::string & filename, const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    write_json_file(filename, run_summary(graph, modules));
}

} // namespace markov::trace_graph::cli
