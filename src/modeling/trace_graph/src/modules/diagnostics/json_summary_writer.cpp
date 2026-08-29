/**
 * @file
 * @brief Compact Debug projection of HiCache effect, cost, and source attribution.
 */
#include "markov/trace_graph/modules/diagnostics/json_summary_writer.hpp"

#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/diagnostics/summary.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"

#include <nlohmann/json.hpp>

namespace markov::trace_graph::modules::diagnostics {

namespace {

using Json = nlohmann::json;

Json hicache_summary(const hicache::HiCacheModule & module) {
    return Json{
        { "name", "HiCacheModule" },
        { "hicache", Json::parse(hicache::diagnostics::summary_json(module.effect_decisions())) },
    };
}

Json io_resource_summary(const hicache::patch::HiCacheIoResourcePlan & resources) {
    Json costs = Json::array();
    for (const auto & cost : resources.costs) {
        costs.push_back({
            {                                      "effect_id",                                                             cost.effect_id },
            {                                    "effect_type",                 hicache::model::hicache_effect_type_name(cost.effect_type) },
            {                                      "direction",            hicache::model::hicache_transfer_direction_name(cost.direction) },
            {                            "target_effect_state", hicache::model::hicache_target_effect_state_name(cost.target_effect_state) },
            {                           "zero_payload_control",                                                  cost.zero_payload_control },
            {                                "operation_count",                                                       cost.operation_count },
            {                           "effective_page_count",                                                  cost.effective_page_count },
            {                           "effective_byte_count",                                                  cost.effective_byte_count },
            {                    "storage_existing_page_count",                                           cost.storage_existing_page_count },
            {                         "storage_new_page_count",                                                cost.storage_new_page_count },
            {         "storage_existing_operation_page_counts",                                cost.storage_existing_operation_page_counts },
            {                    "storage_existing_byte_count",                                           cost.storage_existing_byte_count },
            {                         "storage_new_byte_count",                                                cost.storage_new_byte_count },
            {                        "bandwidth_bytes_per_sec",                                               cost.bandwidth_bytes_per_sec },
            {                         "calibration_setup_us",                                                cost.calibration_setup_us },
            {                      "calibration_transfer_us",                                             cost.calibration_transfer_us },
            {                                "runtime_scale",                                                       cost.runtime_scale },
            {                 "storage_existing_service_us",                                        cost.storage_existing_service_us },
            {                      "storage_new_service_us",                                             cost.storage_new_service_us },
            {                                    "duration_us",                                                           cost.duration_us },
            {                        "host_control_page_count",                                               cost.host_control_page_count },
            {                   "host_control_operation_count",                                          cost.host_control_operation_count },
            {                          "host_control_fixed_us",                                                 cost.host_control_fixed_us },
            {                           "host_control_page_us",                                                  cost.host_control_page_us },
            {                       "host_control_duration_us",                                              cost.host_control_duration_us },
            {                                 "resource_scope",                                                        cost.resource_scope },
            {                                  "resource_lane",                                                         cost.resource_lane },
            {                            "logical_order_epoch",                                                   cost.logical_order_epoch },
            {                                         "status",                   hicache::patch::hicache_io_cost_status_name(cost.status) },
            {                                         "reason",                                                                cost.reason },
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
        {            "status",            resources.status },
        { "kv_bytes_per_page", resources.kv_bytes_per_page },
        {             "costs",            std::move(costs) },
        { "lane_dependencies", std::move(dependencies) },
    };
}

Json source_attribution_summary(const hicache::patch::HiCacheSourceAttributionCatalog & attribution) {
    Json records = Json::array();
    for (const auto & record : attribution.records) {
        records.push_back({
            {                       "effect_id",                       record.effect_id },
            {            "source_carrier_state", hicache::model::hicache_source_carrier_state_name(record.source_carrier_state) },
            {               "timing_fact_nodes",               record.timing_fact_nodes },
            {           "io_operation_record_ids",           record.io_operation_record_ids },
            {    "source_completed_token_count",    record.source_completed_token_count },
            {    "target_effective_token_count",    record.target_effective_token_count },
        });
    }
    return {
        {  "status", attribution.status },
        { "records", std::move(records) },
    };
}

Json patch_summary(const hicache::HiCacheDagPatchModule & module) {
    const auto & result = module.result();
    return {
        { "name", "HiCacheDagPatchModule" },
        { "hicache_dag_patch",
          {
              {                      "status",                      result.status },
              {   "source_target_same_config",   result.source_target_same_config },
              {       "prefill_effect_status",       result.prefill_effect_status },
              {                "io_resources",                io_resource_summary(result.io_resources) },
              {          "source_attribution",          source_attribution_summary(result.source_attribution) },
          } },
    };
}

} // namespace

std::string module_summary_json(const SimulationModule & module) {
    if (const auto * hicache_module = dynamic_cast<const hicache::HiCacheModule *>(&module)) return hicache_summary(*hicache_module).dump();
    if (const auto * patch_module = dynamic_cast<const hicache::HiCacheDagPatchModule *>(&module)) return patch_summary(*patch_module).dump();
    return Json{
        {           "name",             module.name() },
        { "summary_status", "unsupported_module_type" },
    }
        .dump();
}

} // namespace markov::trace_graph::modules::diagnostics
