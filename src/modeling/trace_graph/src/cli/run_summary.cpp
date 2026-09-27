/**
 * @file
 * @brief Serializes compact graph, simulation, and module run results.
 */
#include "markov/trace_graph/cli/run_summary.hpp"
#include "markov/trace_graph/cli/hicache_observations.hpp"

#include "markov/trace_graph/cli/file_output.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#ifdef DEBUG
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"
#endif
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <ranges>

namespace markov::trace_graph::cli {

namespace {

using Json = nlohmann::json;

Json effect_decision_result(const modules::hicache::model::HiCacheEffectDecisionLedger & ledger) {
    Json result;
    result["status"] = ledger.status;
    result["decision_count"] = ledger.decisions.size();
    result["patchable_count"] = ledger.patchable_count();
    result["unresolved_count"] = ledger.unresolved_count();
    result["decision_coverage"] = ledger.decision_coverage;
    result["missing_facts"] = ledger.missing_facts;
    result["not_patchable_reasons"] = ledger.not_patchable_reasons;
    result["byte_projection_available"] = ledger.byte_projection_available;
    result["kv_bytes_per_page"] = ledger.kv_bytes_per_page;
    result["l2_capacity_pages"] = ledger.l2_capacity_pages;
    result["l2_capacity_bytes"] = ledger.l2_capacity_bytes;
    result["byte_projection_source"] = ledger.byte_projection_source;
    return result;
}

Json phase_work_result(const modules::hicache::model::HiCachePhaseWorkLedger & plan) {
    // Keep target work identities for historical replay admission. Per-node
    // costs remain in the model; retired component score tables are not emitted.
    Json prefills = Json::array();
    for (const auto & item : plan.prefills) {
        prefills.push_back({
            {               "logical_input",               item.logical_input },
            {                  "request_id",                  item.request_id },
            {          "prompt_token_count",          item.prompt_token_count },
            { "reusable_prefix_token_count", item.reusable_prefix_token_count },
            {         "prefill_token_count",         item.prefill_token_count },
        });
    }

    Json decodes = Json::array();
    for (const auto & item : plan.decodes) {
        decodes.push_back({
            {   "logical_input",   item.logical_input },
            {      "request_id",      item.request_id },
            { "iteration_count", item.iteration_count },
        });
    }

    return {
        { "status", plan.status },
        { "prefill_status", plan.prefill_status },
        { "decode_status", plan.decode_status },
        { "cost_status", plan.cost_status },
        { "prefill_feature_covered_count", std::ranges::count_if(plan.prefills, [](const auto & item) { return item.feature_covered; }) },
        { "decode_feature_covered_count", std::ranges::count_if(plan.decodes, [](const auto & item) { return item.feature_covered; }) },
        { "prefill_count", plan.prefills.size() },
        { "decode_count", plan.decodes.size() },
        { "blockers", plan.blockers },
        { "prefills", std::move(prefills) },
        { "decodes", std::move(decodes) },
    };
}


#ifdef DEBUG
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
    const auto & oracle = resources.oracle_cost_replay;
    if (oracle.status != "disabled") {
        result["oracle_cost_replay"] = Json{
            {                "status",                oracle.status },
            {   "required_cost_count",   oracle.required_cost_count },
            {   "supplied_cost_count",   oracle.supplied_cost_count },
            {    "applied_cost_count",    oracle.applied_cost_count },
            {     "oracle_service_us",     oracle.oracle_service_us },
            {    "applied_service_us",    oracle.applied_service_us },
            {     "oracle_control_us",     oracle.oracle_control_us },
            {    "applied_control_us",    oracle.applied_control_us },
            { "effect_identity_exact", oracle.effect_identity_exact },
            { "operation_shape_exact", oracle.operation_shape_exact },
            {   "target_e2e_consumed",   oracle.target_e2e_consumed },
        };
    }
    return result;
}

Json phase_carrier_audit(const modules::hicache::HiCachePhaseCarrierAudit & carrier) {
    return Json{
        {                   "status",                   carrier.status },
        {       "request_rank_count",       carrier.request_rank_count },
        {            "request_count",            carrier.request_count },
        {  "synthetic_carrier_count",  carrier.synthetic_carrier_count },
        { "source_device_node_count", carrier.source_device_node_count },
        { "source_submit_node_count", carrier.source_submit_node_count },
        {         "dependency_count",         carrier.dependency_count },
        {     "owner_conflict_count",     carrier.owner_conflict_count },
        {                 "blockers",                 carrier.blockers },
    };
}

Json dag_patch_result(const modules::hicache::HiCacheDagPatchModule & module) {
    const auto & result = module.result();
    const auto & preparation = result.runtime_preparation;
    std::map<std::string, size_t> preparation_counts;
    for (const auto & call : preparation.calls) ++preparation_counts[call.status];
    std::map<std::string, size_t> required_paths;
    for (const auto & call : preparation.calls)
        if (call.status == "required") ++required_paths[call.path];
    Json preparation_costs = Json::object();
    for (const auto & [pid, paths] : preparation.cost_samples)
        for (const auto & [path, samples] : paths)
            preparation_costs[pid][path] = {
                {      "count",      samples.count },
                { "minimum_us", samples.minimum_us },
                {  "median_us",  samples.median_us },
                { "maximum_us", samples.maximum_us }
            };
    const auto duration_update_count = static_cast<size_t>(
        std::ranges::count_if(result.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::SetNodeDuration; }));
    const auto e2e_eligibility_update_count = static_cast<size_t>(
        std::ranges::count_if(result.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::SetNodeE2eEligibility; }));
    const bool topology_valid = result.shadow_rewrite.topology_valid && result.applied_validation.topology_exact;
    const bool validation_ready = result.source_attribution.status == "ready" && result.shadow_rewrite.status == "ready"
                                  && result.boundary_validation.status == "ready" && result.applied_validation.status == "ready" && topology_valid;
    Json summary{
        {                       "status",result.status                                         },
        {           "phase_patch_status",                          result.phase_patch_status },
        {         "cpu_collective_patch",
         { { "status", result.cpu_collective_patch.status },
         { "observed_rounds", result.cpu_collective_patch.observed_rounds },
         { "planned_rounds", result.cpu_collective_patch.planned_rounds },
         { "source_retained_rounds", result.cpu_collective_patch.source_retained_rounds },
         { "blockers", result.cpu_collective_patch.blockers } }                             },
        {             "layer_wait_patch",
         { { "status", result.layer_wait_patch.status },
         { "retained_calls", result.layer_wait_patch.retained_calls },
         { "removed_calls", result.layer_wait_patch.removed_calls },
         { "required_insertions", result.layer_wait_patch.required_insertions },
         { "blockers", result.layer_wait_patch.blockers },
         { "inserted_calls", result.layer_wait_patch.inserted_calls },
         { "rebound_calls", result.layer_wait_patch.rebound_calls },
         { "expanded_loadbacks", result.layer_wait_patch.expanded_loadbacks },
         { "removed_main_cpu_us", result.layer_wait_patch.removed_main_cpu_us },
         { "removed_worker_us", result.layer_wait_patch.removed_worker_us },
         { "added_return_us", result.layer_wait_patch.added_return_us },
         { "inactive_call_median_ns", result.layer_wait_patch.inactive_call_median_ns } }   },
        {          "runtime_preparation",
         { { "status", preparation.status },
         { "call_counts", preparation_counts },
         { "observed_formal_calls", preparation.observed_formal_calls },
         { "blockers", preparation.blockers },
         { "changed_gap_count", preparation.mutation.set_cpu_gaps.size() },
         { "removed_coverage_us", preparation.removed_coverage_us },
         { "added_cost_us", preparation.added_cost_us },
         { "required_path_counts", required_paths },
         { "source_parallel_compilation", preparation.source_parallel_compilation },
         { "cost_samples", preparation_costs },
         { "estimate_assumption", "fresh shared disk cache; parallel first use within each batch" },
         { "cost_source", preparation.added_cost_us ? "selected_base_path_medians" : "source_prepare_load_interval_union" },
         { "coverage_is_e2e_saving", false } }                                              },
        {  "phase_duration_update_count",                 result.phase_duration_update_count },
        {   "phase_owner_conflict_count",                  result.phase_owner_conflict_count },
        {                    "component",                           result.journal.component },
        {               "mutation_count",                      result.journal.records.size() },
        {        "duration_update_count",                              duration_update_count },
        { "e2e_eligibility_update_count",                       e2e_eligibility_update_count },
        {          "active_nodes_before",                 result.journal.active_nodes_before },
        {           "active_nodes_after",                  result.journal.active_nodes_after },
        {          "active_edges_before",                 result.journal.active_edges_before },
        {           "active_edges_after",                  result.journal.active_edges_after },
        {                 "io_resources",            io_resource_result(result.io_resources) },
        {                "phase_carrier",          phase_carrier_audit(result.phase_carrier) },
        {               "topology_valid",                                     topology_valid },
        {               "blocker_counts",                              result.apply_blockers },
        {       "rewrite_counts_by_kind",       result.shadow_rewrite.counts_by_rewrite_kind },
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
         }                                                                                  },
    };
    if (result.phase_oracle_cost_replay.status != "disabled") {
        const auto & oracle = result.phase_oracle_cost_replay;
        summary["phase_oracle_cost_replay"] = Json{
            {                "status",                oracle.status },
            {   "required_cost_count",   oracle.required_cost_count },
            {   "supplied_cost_count",   oracle.supplied_cost_count },
            {    "applied_cost_count",    oracle.applied_cost_count },
            {    "oracle_duration_us",    oracle.oracle_duration_us },
            {   "applied_duration_us",   oracle.applied_duration_us },
            { "effect_identity_exact", oracle.effect_identity_exact },
            {   "target_e2e_consumed",   oracle.target_e2e_consumed },
        };
    }
    return summary;
}
#endif

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
                { "effect_decisions", effect_decision_result(hicache->effect_decisions()) },
                {       "phase_work",            phase_work_result(hicache->phase_work()) },
            };
        }
#ifdef DEBUG
        else if (const auto * patch = dynamic_cast<const modules::hicache::HiCacheDagPatchModule *>(module.get())) {
            results["hicache_dag_patch"] = dag_patch_result(*patch);
        }
        else if (const auto * observed = dynamic_cast<const modules::hicache::HiCacheObservedPhaseCarrierModule *>(module.get())) {
            const auto & result = observed->result();
            results["hicache_observed_phase_carrier"] = Json{
                {               "status",result.status                                         },
                {       "direct_carrier",
                 Json{
                 { "status", result.direct.status },
                 { "observed_operation_count", result.direct.observed_operation_count },
                 { "canonical_prefetch_count", result.direct.canonical_prefetch_count },
                 { "synthetic_node_count", result.direct.synthetic_node_count },
                 { "dependency_count", result.direct.dependency_count },
                 { "blockers", result.direct.blockers },
                 }                                                           },
                {              "carrier", phase_carrier_audit(result.carrier) },
                {       "mutation_count",       result.journal.records.size() },
                {  "active_nodes_before",  result.journal.active_nodes_before },
                {   "active_nodes_after",   result.journal.active_nodes_after },
                {  "active_edges_before",  result.journal.active_edges_before },
                {   "active_edges_after",   result.journal.active_edges_after },
                {       "topology_valid",                result.topology.ok() },
                { "topology_issue_count",       result.topology.issues.size() },
            };
        }
#endif
    }
    return results;
}

Json run_summary(const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules,
                 const modules::hicache::HiCachePhaseObservationAudit & phase, bool include_source_observations, bool executing_hicache) {
    const auto stats = graph.summary_stats();
    Json root;
    root["parsed_record_count"] = graph.parsed_record_count();
    root["simulated_e2e_us"] = graph.e2e_time();
    // Execution has no target ownership projection yet. Its component fields
    // are marked unavailable below. Ordinary DAG builds do not request them.
    if (include_source_observations && !executing_hicache) {
        root["simulated_control_e2e_us"] = graph.control_e2e_time();
        root["simulated_gap_excluded_e2e_us"] = graph.gap_excluded_e2e_time();
        uint64_t scope_node_duration_us = 0;
        uint64_t scope_gap_duration_us = 0;
        size_t scope_owned_node_count = 0;
        for (const auto & node : graph.nodes()) {
            if (!node.active) continue;
            if (graph.scope_node_owned(node.id)) {
                ++scope_owned_node_count;
                scope_node_duration_us = core::checked_add_u64(scope_node_duration_us, node.duration, "scope-owned node duration exceeds uint64 range");
            }
            scope_gap_duration_us =
                core::checked_add_u64(scope_gap_duration_us, graph.scope_gap_duration(node.id), "scope-owned gap duration exceeds uint64 range");
        }
        root["scope_owned_node_count"] = scope_owned_node_count;
        root["scope_owned_node_duration_us"] = scope_node_duration_us;
        root["scope_owned_gap_duration_us"] = scope_gap_duration_us;
    }
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
    root["source_observations_status"] = include_source_observations ? "included" : "not_requested";
    if (include_source_observations) root["source_phase_observations"] = hicache_phase_observations(phase);
    root["module_results"] = module_results(modules);
    return root;
}

} // namespace

void write_run_summary(const std::string & filename, const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules,
                       const nlohmann::json & source_io_observations, const modules::hicache::HiCachePhaseObservationAudit & source_phase_observations,
                       const nlohmann::json & client_result, const modules::hicache::runtime::HiCacheWindowResult * execution,
                       bool include_source_observations) {
    auto summary = run_summary(graph, modules, source_phase_observations, include_source_observations, execution != nullptr);
    if (include_source_observations) summary["source_io_observations"] = source_io_observations;
    summary["http_client"] = client_result;
    if (execution) {
        const auto & result = *execution;
        if (include_source_observations)
            summary["module_results"]["hicache"] = {
                { "effect_decisions", effect_decision_result(result.model.effect_decisions) },
                {       "phase_work",            phase_work_result(result.model.phase_work) }
            };
        summary["module_results"]["hicache_execution"] = {
            {                             "status",                                  "executed"                                                   },
            {                      "cost_coverage",                                                                                      "partial" },
            {                            "http_us",                                                                                 result.http_us },
            {                     "prepared_facts",                                                                          result.prepared_facts },
            {                     "consumed_facts",                                                                          result.consumed_facts },
            {              "wait_template_request",                                                                   result.wait_template_request },
            {          "wait_calibration_manifest",                                                               result.wait_calibration_manifest },
            { "local_return_calibration_manifests",                                                      result.local_return_calibration_manifests },
            {         "query_calibration_manifest",                                                              result.query_calibration_manifest },
            {                 "decode_allocations",                                                                      result.decode_allocations },
            {                   "phase_admissions",                                                                        result.phase_admissions },
            {                  "changed_operators",                                                                       result.changed_operators },
            {            "source_load_submissions",                                                                 result.source_load_submissions },
            {             "completed_load_batches",                                                                  result.completed_load_batches },
            {                        "layer_calls",
             { { "prepared", result.prepared_layer_calls }, { "active", result.active_layer_calls }, { "inactive", result.inactive_layer_calls } } },
            {           "source_write_submissions",                                                                result.source_write_submissions },
            {                   "completed_writes",                                                                        result.completed_writes },
            {                "resumed_allocations",                                                                     result.resumed_allocations },
            {           "prepared_write_templates",                                                                result.prepared_write_templates },
            {         "prepared_eviction_controls",                                                              result.prepared_eviction_controls },
            {         "expanded_write_submissions",                                                              result.expanded_write_submissions },
            {            "calibrated_empty_checks",                                                                 result.calibrated_empty_checks },
            {            "generated_control_steps",                                                                 result.generated_control_steps },
            {          "generated_blocking_checks",                                                               result.generated_blocking_checks },
            { "blocking_check_cost_extrapolations",                                                      result.blocking_check_cost_extrapolations },
            {   "phase_extrapolated_control_steps",                                                        result.phase_extrapolated_control_steps },
            {      "calibrated_empty_check_cpu_us",                                                           result.calibrated_empty_check_cpu_us },
            {                  "preparation_slots",                                                                       result.preparation_slots },
            {              "executed_preparations",                                                                   result.executed_preparations },
            {                      "confirmations",
             { { "load_rounds", result.confirmations.load_rounds },
             { "write_rounds", result.confirmations.write_rounds },
             { "storage_rounds", result.confirmations.storage_rounds },
             { "replaced_rounds", result.confirmations.replaced_rounds },
             { "partial_window_rounds", result.confirmations.partial_window_rounds } }                                                            },
            {                    "cpu_collectives",
             { { "status", result.cpu_collectives.status },
             { "observed", result.cpu_collectives.observed_rounds },
             { "planned", result.cpu_collectives.planned_rounds },
             { "retained", result.cpu_collectives.source_retained_rounds } }                                                                      },
            {           "remaining_approximations",                   { "source_cpu_branch_costs", "writeback_blocking", "source_residual_waits" } }
        };
        // Full HTTP includes every retained cost. Target ownership projections
        // are not implemented yet; zero would falsely look like a valid score.
        summary["component_metrics_status"] = "unavailable_target_ownership";
        for (const auto key : { "simulated_control_e2e_us",
                                "simulated_gap_excluded_e2e_us",
                                "scope_owned_node_count",
                                "scope_owned_node_duration_us",
                                "scope_owned_gap_duration_us" })
            summary[key] = nullptr;
    }
    write_json_file(filename, summary);
}

} // namespace markov::trace_graph::cli
