/**
 * @file
 * @brief Serializes compact graph, simulation, and module run results.
 */
#include "run_summary.hpp"

#include "file_output.hpp"

#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"

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
    result["prefetch_readiness_status"] = ledger.prefetch_readiness_status;
    result["counts_by_effect_type"] = ledger.counts_by_effect_type();
    result["counts_by_target_effect_state"] = ledger.counts_by_target_effect_state();
    result["counts_by_schedule_sensitivity"] = ledger.counts_by_schedule_sensitivity();
    result["counts_by_source_carrier_state"] = ledger.counts_by_source_carrier_state();
    result["missing_facts"] = ledger.missing_facts;
    result["not_patchable_reasons"] = ledger.not_patchable_reasons;
    result["byte_projection_available"] = ledger.byte_projection_available;
    result["kv_bytes_per_page"] = ledger.kv_bytes_per_page;
    result["byte_projection_source"] = ledger.byte_projection_source;
    result["io_model_id"] = ledger.io_model_id;
    result["io_model_digest"] = ledger.io_model_digest;
    result["io_model_calibration_status"] = ledger.io_model_calibration_status;
    result["resource_model_id"] = ledger.resource_model;
    result["device_host_bandwidth_parameter_present"] = ledger.device_host_bandwidth_bytes_per_sec > 0;
    result["host_storage_bandwidth_parameter_present"] = ledger.host_storage_bandwidth_bytes_per_sec > 0;
    return result;
}

Json io_resource_result(const modules::hicache::patch::HiCacheIoResourcePlan & resources) {
    return Json{
        {                                   "status",                                   resources.status },
        {                              "io_model_id",                              resources.io_model_id },
        {                          "io_model_digest",                          resources.io_model_digest },
        {              "io_model_calibration_status",              resources.io_model_calibration_status },
        {                    "io_model_allows_apply",                   resources.calibrated_for_apply() },
        {                        "resource_model_id",                        resources.resource_model_id },
        {                   "byte_projection_source",                   resources.byte_projection_source },
        {                        "kv_bytes_per_page",                        resources.kv_bytes_per_page },
        {  "device_host_bandwidth_parameter_present",  resources.device_host_bandwidth_bytes_per_sec > 0 },
        { "host_storage_bandwidth_parameter_present", resources.host_storage_bandwidth_bytes_per_sec > 0 },
        {                           "decision_count",                             resources.costs.size() },
        {                         "cost_ready_count",                            resources.ready_count() },
        {                    "lane_dependency_count",                 resources.lane_dependencies.size() },
        {                    "counts_by_cost_status",                         resources.counts_by_status },
        {                           "blocker_counts",                           resources.blocker_counts },
    };
}

Json source_index_result(const modules::hicache::patch::HiCacheSourceDagIndexStats & index) {
    return Json{
        {                                 "status",                                 index.status },
        {                      "stored_node_count",                      index.stored_node_count },
        {                      "active_node_count",                      index.active_node_count },
        {                      "active_edge_count",                      index.active_edge_count },
        {                        "fact_node_count",                        index.fact_node_count },
        {           "workload_identity_fact_count",           index.workload_identity_fact_count },
        {               "source_actual_fact_count",               index.source_actual_fact_count },
        {          "timing_observation_fact_count",          index.timing_observation_fact_count },
        {                   "malformed_fact_count",                   index.malformed_fact_count },
        {                 "request_identity_count",                 index.request_identity_count },
        {               "operation_identity_count",               index.operation_identity_count },
        {               "dag_patch_contract_ready",               index.dag_patch_contract_ready },
        {                   "counts_by_fact_class",                   index.counts_by_fact_class },
        {                    "counts_by_fact_role",                    index.counts_by_fact_role },
        {   "request_identity_counts_by_fact_role",   index.request_identity_counts_by_fact_role },
        { "operation_identity_counts_by_fact_role", index.operation_identity_counts_by_fact_role },
    };
}

Json source_attribution_result(const modules::hicache::patch::HiCacheSourceAttributionCatalog & attribution) {
    return Json{
        {                         "status",                         attribution.status },
        {                 "decision_count",                 attribution.records.size() },
        {               "attributed_count",             attribution.attributed_count() },
        {               "unresolved_count",             attribution.unresolved_count() },
        { "counts_by_source_carrier_state", attribution.counts_by_source_carrier_state },
        {          "counts_by_effect_type",          attribution.counts_by_effect_type },
        {                 "blocker_counts",                 attribution.blocker_counts },
    };
}

Json shadow_rewrite_result(const modules::hicache::patch::HiCacheShadowRewriteTransaction & shadow) {
    return Json{
        {                      "status",                         shadow.status },
        { "io_model_calibration_status",    shadow.io_model_calibration_status },
        {       "io_model_allows_apply",          shadow.io_model_allows_apply },
        {              "decision_count",               shadow.decisions.size() },
        {                 "ready_count",                  shadow.ready_count() },
        {              "rejected_count",               shadow.rejected_count() },
        {           "shadow_plan_empty",                   shadow.plan.empty() },
        {       "shadow_topology_valid",                 shadow.topology_valid },
        {       "duration_update_count", shadow.plan.set_node_durations.size() },
        {        "synthetic_node_count",    shadow.plan.synthetic_nodes.size() },
        {            "added_edge_count",          shadow.plan.add_edges.size() },
        {      "counts_by_rewrite_kind",         shadow.counts_by_rewrite_kind },
        {              "blocker_counts",                 shadow.blocker_counts },
        {    "ownership_conflict_count",     shadow.ownership_conflicts.size() },
    };
}

Json boundary_validation_result(const modules::hicache::patch::HiCacheBoundaryValidationCatalog & validation) {
    return Json{
        {         "status",         validation.status },
        { "decision_count", validation.records.size() },
        {    "ready_count",  validation.ready_count() },
        { "blocker_counts", validation.blocker_counts },
    };
}

Json applied_validation_result(const modules::hicache::patch::HiCacheAppliedPatchValidation & validation) {
    return Json{
        {                            "status",                            validation.status },
        {                    "decision_count",                    validation.records.size() },
        {                       "ready_count",                     validation.ready_count() },
        {                "plan_journal_exact",                validation.plan_journal_exact },
        { "prospective_materialization_exact", validation.prospective_materialization_exact },
        {                    "topology_exact",                    validation.topology_exact },
        {         "family_dependencies_exact",         validation.family_dependencies_exact },
        {           "lane_dependencies_exact",           validation.lane_dependencies_exact },
        {                    "blocker_counts",                    validation.blocker_counts },
    };
}

Json dag_patch_result(const modules::hicache::HiCacheDagPatchModule & module) {
    const auto & result = module.result();
    const auto duration_update_count = static_cast<size_t>(
        std::ranges::count_if(result.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::SetNodeDuration; }));
    Json summary{
        {                    "status",                                          result.status },
        {     "prefill_effect_status",                           result.prefill_effect_status },
        { "prefetch_readiness_status",                       result.prefetch_readiness_status },
        {                   "plan_id",                                    result.plan.plan_id },
        {            "mutation_count",                          result.journal.records.size() },
        {     "duration_update_count",                                  duration_update_count },
        {       "active_nodes_before",                     result.journal.active_nodes_before },
        {        "active_nodes_after",                      result.journal.active_nodes_after },
        {       "active_edges_before",                     result.journal.active_edges_before },
        {        "active_edges_after",                      result.journal.active_edges_after },
        {              "io_resources",                io_resource_result(result.io_resources) },
        {              "source_index",               source_index_result(result.source_index) },
        {        "source_attribution",   source_attribution_result(result.source_attribution) },
        {            "shadow_rewrite",           shadow_rewrite_result(result.shadow_rewrite) },
        {       "boundary_validation", boundary_validation_result(result.boundary_validation) },
        {        "applied_validation",   applied_validation_result(result.applied_validation) },
        {            "apply_blockers",                                  result.apply_blockers },
    };
#ifdef DEBUG
    summary["topology_valid"] = result.topology.ok();
    summary["stage_timings_ms"] = Json{
        {         "io_resource_ms",         result.timings.io_resource_ms },
        {        "source_index_ms",        result.timings.source_index_ms },
        {  "source_attribution_ms",  result.timings.source_attribution_ms },
        {    "rewrite_planning_ms",    result.timings.rewrite_planning_ms },
        { "boundary_validation_ms", result.timings.boundary_validation_ms },
        {      "mutation_apply_ms",      result.timings.mutation_apply_ms },
        {  "applied_validation_ms",  result.timings.applied_validation_ms },
        {               "total_ms",               result.timings.total_ms },
    };
#endif
    return summary;
}

Json module_results(const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    Json results = Json::object();
    for (const auto & module : modules) {
        if (const auto * hicache = dynamic_cast<const modules::hicache::HiCacheModule *>(module.get())) {
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
    root["schema"] = "markov.trace_graph.run_summary.v3";
    root["parsed_record_count"] = graph.parsed_record_count();
    root["simulated_e2e_us"] = graph.e2e_time();
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

#ifdef DEBUG
void write_run_summary(const std::string & filename, const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules,
                       const nlohmann::json & stage_timings) {
    auto root = run_summary(graph, modules);
    root["stage_timings_ms"] = stage_timings;
    write_json_file(filename, root);
}
#endif

} // namespace markov::trace_graph::cli
