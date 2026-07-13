/**
 * @file
 * @brief JSON serializer for SimulationModule diagnostics summaries.
 */
#include "markov/trace_graph/modules/diagnostics/json_summary_writer.hpp"

#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/diagnostics/summary.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

#include <nlohmann/json.hpp>

namespace markov::trace_graph::modules::diagnostics {

namespace json_summary_writer_detail {

using Json = nlohmann::json;

Json node_scale_summary_json(const node_scale::NodeScaleModule & module) {
    const auto summary = module.summary();
    Json root{
        {         "name",           "NodeScaleModule" },
        {   "rule_count", summary.config.rules.size() },
        { "scaled_nodes",        summary.scaled_nodes },
    };
    root["rules"] = Json::array();
    for (const auto & rule : summary.config.rules) {
        root["rules"].push_back({
            {     "id",     rule.id },
            {   "name",   rule.name },
            { "factor", rule.factor },
        });
    }
    return root;
}

Json hicache_summary_json(const hicache::HiCacheModule & module) {
    return Json{
        { "name", "HiCacheModule" },
        { "hicache", Json::parse(hicache::diagnostics::summary_json(module.summary(), module.effect_decisions())) },
    };
}

Json topology_report_json(const core::DagTopologyValidationReport & report) {
    Json issues = Json::array();
    for (const auto & issue : report.issues) {
        issues.push_back({
            {         "code",         issue.code },
            {      "message",      issue.message },
            {     "node_ids",     issue.node_ids },
            { "edge_indices", issue.edge_indices },
        });
    }
    return {
        {             "valid",              report.ok() },
        { "active_node_count", report.active_node_count },
        { "active_edge_count", report.active_edge_count },
        {       "cycle_nodes",       report.cycle_nodes },
        {            "issues",        std::move(issues) },
    };
}

Json optional_size_json(const std::optional<size_t> & value) { return value ? Json(*value) : Json(nullptr); }

Json io_resource_json(const hicache::patch::HiCacheIoResourcePlan & resources) {
    Json costs = Json::array();
    for (const auto & cost : resources.costs) {
        costs.push_back({
            {               "effect_id",                                                             cost.effect_id },
            {             "effect_type",                 hicache::model::hicache_effect_type_name(cost.effect_type) },
            {               "direction",            hicache::model::hicache_transfer_direction_name(cost.direction) },
            {     "target_effect_state", hicache::model::hicache_target_effect_state_name(cost.target_effect_state) },
            {    "effective_byte_count",                                                  cost.effective_byte_count },
            {         "bandwidth_field",                                                       cost.bandwidth_field },
            { "bandwidth_bytes_per_sec",                                               cost.bandwidth_bytes_per_sec },
            {             "duration_us",                                                           cost.duration_us },
            {          "resource_scope",                                                        cost.resource_scope },
            {           "resource_lane",                                                         cost.resource_lane },
            {     "logical_order_epoch",                                                   cost.logical_order_epoch },
            {                  "status",                   hicache::patch::hicache_io_cost_status_name(cost.status) },
            {                  "reason",                                                                cost.reason },
        });
    }
    Json dependencies = Json::array();
    for (const auto & dependency : resources.lane_dependencies) {
        dependencies.push_back({
            {         "resource_lane",         dependency.resource_lane },
            { "predecessor_effect_id", dependency.predecessor_effect_id },
            {   "successor_effect_id",   dependency.successor_effect_id },
        });
    }
    return {
        {                               "status",                               resources.status },
        {                          "io_model_id",                          resources.io_model_id },
        {                      "io_model_digest",                      resources.io_model_digest },
        {          "io_model_calibration_status",          resources.io_model_calibration_status },
        {                "io_model_allows_apply",               resources.calibrated_for_apply() },
        {                    "resource_model_id",                    resources.resource_model_id },
        {               "byte_projection_source",               resources.byte_projection_source },
        {                    "kv_bytes_per_page",                    resources.kv_bytes_per_page },
        {  "device_host_bandwidth_bytes_per_sec",  resources.device_host_bandwidth_bytes_per_sec },
        { "host_storage_bandwidth_bytes_per_sec", resources.host_storage_bandwidth_bytes_per_sec },
        {                  "io_model_provenance",                  resources.io_model_provenance },
        {                "counts_by_cost_status",                     resources.counts_by_status },
        {                       "blocker_counts",                       resources.blocker_counts },
        {                                "costs",                               std::move(costs) },
        {                    "lane_dependencies",                        std::move(dependencies) },
    };
}

Json source_index_json(const hicache::patch::HiCacheSourceDagIndexStats & index) {
    return {
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

Json source_attribution_json(const hicache::patch::HiCacheSourceAttributionCatalog & attribution) {
    Json records = Json::array();
    for (const auto & record : attribution.records) {
        records.push_back({
            {              "effect_id",                                                               record.effect_id },
            {            "effect_type",                   hicache::model::hicache_effect_type_name(record.effect_type) },
            {    "target_effect_state",   hicache::model::hicache_target_effect_state_name(record.target_effect_state) },
            {   "source_carrier_state", hicache::model::hicache_source_carrier_state_name(record.source_carrier_state) },
            {    "source_fact_node_id",                                                     record.source_fact_node_id },
            {        "identity_method",                                                         record.identity_method },
            {               "evidence",                                                                record.evidence },
            {     "control_fact_nodes",                                                      record.control_fact_nodes },
            {      "timing_fact_nodes",                                                       record.timing_fact_nodes },
            {  "operation_chain_nodes",                                                   record.operation_chain_nodes },
            {          "carrier_nodes",                                                           record.carrier_nodes },
            {   "owned_duration_nodes",                                                    record.owned_duration_nodes },
            { "carrier_internal_edges",                                                  record.carrier_internal_edges },
            {    "carrier_entry_edges",                                                     record.carrier_entry_edges },
            {     "carrier_exit_edges",                                                      record.carrier_exit_edges },
            {           "start_anchor",                                        optional_size_json(record.start_anchor) },
            {      "completion_anchor",                                   optional_size_json(record.completion_anchor) },
            {       "consumer_anchors",                                                        record.consumer_anchors },
            { "consumer_anchor_method",                                                  record.consumer_anchor_method },
            {                 "reason",                                                                  record.reason },
        });
    }
    return {
        {                         "status",                         attribution.status },
        {                 "decision_count",                 attribution.records.size() },
        {               "attributed_count",             attribution.attributed_count() },
        {               "unresolved_count",             attribution.unresolved_count() },
        { "counts_by_source_carrier_state", attribution.counts_by_source_carrier_state },
        {          "counts_by_effect_type",          attribution.counts_by_effect_type },
        {                 "blocker_counts",                 attribution.blocker_counts },
        {                        "records",                         std::move(records) },
    };
}

Json shadow_rewrite_json(const hicache::patch::HiCacheShadowRewriteTransaction & shadow) {
    Json decisions = Json::array();
    for (const auto & decision : shadow.decisions) {
        decisions.push_back({
            {              "effect_id",                                                               decision.effect_id },
            {       "effect_family_id",                                                        decision.effect_family_id },
            {            "effect_type",                   hicache::model::hicache_effect_type_name(decision.effect_type) },
            {    "target_effect_state",   hicache::model::hicache_target_effect_state_name(decision.target_effect_state) },
            {   "source_carrier_state", hicache::model::hicache_source_carrier_state_name(decision.source_carrier_state) },
            {           "rewrite_kind",                 hicache::patch::hicache_rewrite_kind_name(decision.rewrite_kind) },
            {      "shadow_plan_ready",                                                       decision.shadow_plan_ready },
            {            "duration_us",                                                             decision.duration_us },
            {          "resource_lane",                                                           decision.resource_lane },
            {           "synthetic_id",                                                            decision.synthetic_id },
            {          "carrier_nodes",                                                           decision.carrier_nodes },
            {   "owned_duration_nodes",                                                    decision.owned_duration_nodes },
            {    "carrier_entry_edges",                                                     decision.carrier_entry_edges },
            {     "carrier_exit_edges",                                                      decision.carrier_exit_edges },
            {    "source_fact_node_id",                                                     decision.source_fact_node_id },
            {       "consumer_anchors",                                                        decision.consumer_anchors },
            { "consumer_anchor_method",                                                  decision.consumer_anchor_method },
            {                 "reason",                                                                  decision.reason },
            {                "blocker",                                                                 decision.blocker },
        });
    }
    return {
        {                        "status",                         shadow.status },
        {   "io_model_calibration_status",    shadow.io_model_calibration_status },
        {         "io_model_allows_apply",          shadow.io_model_allows_apply },
        {                "decision_count",               shadow.decisions.size() },
        {                   "ready_count",                  shadow.ready_count() },
        {                "rejected_count",               shadow.rejected_count() },
        {        "counts_by_rewrite_kind",         shadow.counts_by_rewrite_kind },
        {                "blocker_counts",                 shadow.blocker_counts },
        {           "ownership_conflicts",            shadow.ownership_conflicts },
        {             "shadow_plan_empty",                   shadow.plan.empty() },
        {         "duration_update_count", shadow.plan.set_node_durations.size() },
        {          "synthetic_node_count",    shadow.plan.synthetic_nodes.size() },
        {              "added_edge_count",          shadow.plan.add_edges.size() },
        {         "shadow_topology_valid",                 shadow.topology_valid },
        { "prospective_active_node_count",  shadow.prospective_active_node_count },
        { "prospective_active_edge_count",  shadow.prospective_active_edge_count },
        {                      "topology", topology_report_json(shadow.topology) },
        {                     "decisions",                  std::move(decisions) },
    };
}

Json applied_validation_json(const hicache::patch::HiCacheAppliedPatchValidation & validation) {
    Json records = Json::array();
    for (const auto & record : validation.records) {
        records.push_back({
            {                 "effect_id",                                               record.effect_id },
            {              "rewrite_kind", hicache::patch::hicache_rewrite_kind_name(record.rewrite_kind) },
            {                     "ready",                                                   record.ready },
            {     "source_duration_exact",                                   record.source_duration_exact },
            {      "synthetic_cost_exact",                                    record.synthetic_cost_exact },
            {             "ingress_exact",                                           record.ingress_exact },
            { "consumer_dependency_exact",                               record.consumer_dependency_exact },
            {                    "reason",                                                  record.reason },
        });
    }
    return {
        {                            "status",                            validation.status },
        {                    "decision_count",                    validation.records.size() },
        {                       "ready_count",                     validation.ready_count() },
        {                "plan_journal_exact",                validation.plan_journal_exact },
        { "prospective_materialization_exact", validation.prospective_materialization_exact },
        {                    "topology_exact",                    validation.topology_exact },
        {         "family_dependencies_exact",         validation.family_dependencies_exact },
        {           "lane_dependencies_exact",           validation.lane_dependencies_exact },
        {                    "blocker_counts",                    validation.blocker_counts },
        {                           "records",                           std::move(records) },
    };
}

Json boundary_validation_json(const hicache::patch::HiCacheBoundaryValidationCatalog & validation) {
    Json records = Json::array();
    for (const auto & record : validation.records) {
        records.push_back({
            {                 "effect_id",                                               record.effect_id },
            {              "rewrite_kind", hicache::patch::hicache_rewrite_kind_name(record.rewrite_kind) },
            {                     "ready",                                                   record.ready },
            {       "source_cost_removed",                                     record.source_cost_removed },
            {  "target_cost_materialized",                                record.target_cost_materialized },
            {         "ingress_preserved",                                       record.ingress_preserved },
            {          "egress_preserved",                                        record.egress_preserved },
            { "consumer_dependency_ready",                               record.consumer_dependency_ready },
            {                    "reason",                                                  record.reason },
        });
    }
    return {
        {         "status",         validation.status },
        { "decision_count", validation.records.size() },
        {    "ready_count",  validation.ready_count() },
        { "blocker_counts", validation.blocker_counts },
        {        "records",        std::move(records) },
    };
}

Json patch_summary_json(const hicache::HiCacheDagPatchModule & module) {
    const auto & result = module.result();
    Json records = Json::array();
    for (const auto & record : result.journal.records) {
        records.push_back({
            {              "action",                    core::dag_mutation_action_name(record.action) },
            {           "effect_id",                                                 record.effect_id },
            {              "reason",                                                    record.reason },
            {             "node_id",                               optional_size_json(record.node_id) },
            {          "edge_index",                            optional_size_json(record.edge_index) },
            { "replaced_edge_index",                   optional_size_json(record.replaced_edge_index) },
            {                 "src",                                   optional_size_json(record.src) },
            {                 "dst",                                   optional_size_json(record.dst) },
            {        "old_duration", record.old_duration ? Json(*record.old_duration) : Json(nullptr) },
            {        "new_duration", record.new_duration ? Json(*record.new_duration) : Json(nullptr) },
        });
    }

    Json plan{
        {               "plan_id",                   result.plan.plan_id },
        {             "effect_id",                 result.plan.effect_id },
        {                "reason",                    result.plan.reason },
        { "duration_update_count", result.plan.set_node_durations.size() },
        {    "disable_node_count",      result.plan.disable_nodes.size() },
        {    "disable_edge_count",      result.plan.disable_edges.size() },
        {  "synthetic_node_count",    result.plan.synthetic_nodes.size() },
        {        "add_edge_count",          result.plan.add_edges.size() },
        {   "redirect_edge_count",     result.plan.redirect_edges.size() },
    };
    Json journal{
        {             "plan_id",             result.journal.plan_id },
        {      "mutation_count",      result.journal.records.size() },
        { "active_nodes_before", result.journal.active_nodes_before },
        {  "active_nodes_after",  result.journal.active_nodes_after },
        { "active_edges_before", result.journal.active_edges_before },
        {  "active_edges_after",  result.journal.active_edges_after },
        {             "records",                 std::move(records) },
    };
    return {
        {              "name","HiCacheDagPatchModule" },
        { "hicache_dag_patch",
         {
         { "status", result.status },
         { "prefill_effect_status", result.prefill_effect_status },
         { "prefetch_readiness_status", result.prefetch_readiness_status },
         { "io_resources", io_resource_json(result.io_resources) },
         { "source_index", source_index_json(result.source_index) },
         { "source_attribution", source_attribution_json(result.source_attribution) },
         { "shadow_rewrite", shadow_rewrite_json(result.shadow_rewrite) },
         { "boundary_validation", boundary_validation_json(result.boundary_validation) },
         { "applied_validation", applied_validation_json(result.applied_validation) },
         { "apply_blockers", result.apply_blockers },
         { "plan", std::move(plan) },
         { "journal", std::move(journal) },
         { "topology", topology_report_json(result.topology) },
         { "stage_timings_ms",
         {
         { "io_resource_ms", result.timings.io_resource_ms },
         { "source_index_ms", result.timings.source_index_ms },
         { "source_attribution_ms", result.timings.source_attribution_ms },
         { "rewrite_planning_ms", result.timings.rewrite_planning_ms },
         { "boundary_validation_ms", result.timings.boundary_validation_ms },
         { "mutation_apply_ms", result.timings.mutation_apply_ms },
         { "applied_validation_ms", result.timings.applied_validation_ms },
         { "total_ms", result.timings.total_ms },
         } },
         }              },
    };
}

} // namespace json_summary_writer_detail

using json_summary_writer_detail::hicache_summary_json;
using json_summary_writer_detail::Json;
using json_summary_writer_detail::node_scale_summary_json;
using json_summary_writer_detail::patch_summary_json;

/**
 * @brief Serializes a diagnostics summary according to the module's concrete type.
 *
 * Summaries are explicit Debug/validation artifacts. Unknown modules retain a stable JSON
 * object so adding a business module does not break the CLI serialization path.
 */
std::string module_summary_json(const SimulationModule & module) {
    if (const auto * node_scale = dynamic_cast<const node_scale::NodeScaleModule *>(&module)) return node_scale_summary_json(*node_scale).dump();
    if (const auto * hicache = dynamic_cast<const hicache::HiCacheModule *>(&module)) return hicache_summary_json(*hicache).dump();
    if (const auto * patch = dynamic_cast<const hicache::HiCacheDagPatchModule *>(&module)) return patch_summary_json(*patch).dump();

    return Json{
        {           "name",             module.name() },
        { "summary_status", "unsupported_module_type" },
    }
        .dump();
}

} // namespace markov::trace_graph::modules::diagnostics
