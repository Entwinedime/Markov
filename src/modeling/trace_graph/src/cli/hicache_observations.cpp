/** @file Serializes the narrow source observations consumed by Python modeling. */
#include "markov/trace_graph/cli/hicache_observations.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::cli {

namespace {
using Json = nlohmann::json;
Json phase_cost_families(const std::map<std::string, modules::hicache::HiCachePhaseCostFamily> & families) {
    Json result = Json::object();
    for (const auto & [name, family] : families) {
        result[name] = Json{
            {  "node_count",  family.node_count },
            { "duration_us", family.duration_us }
        };
    }
    return result;
}
} // namespace

nlohmann::json hicache_phase_observations(const modules::hicache::HiCachePhaseObservationAudit & phase) {
    using Json = nlohmann::json;
    Json phase_rows = Json::array();
    for (const auto & observation : phase.observations) {
        const auto prefill_kernel_count = observation.prefill_common_kernel_node_ids.size() + observation.prefill_prefix_attention_node_ids.size();

        phase_rows.push_back(Json{
            {                        "logical_input",                                observation.logical_input },
            {                                  "pid",                                          observation.pid },
            {                          "request_ids",                                  observation.request_ids },
            {                           "batch_size",                                   observation.batch_size },
            {                     "source_page_size",                             observation.source_page_size },
            {                   "prompt_token_count",                           observation.prompt_token_count },
            {                  "prefill_token_count",                          observation.prefill_token_count },
            {                     "prefill_start_us",                             observation.prefill_start_us },
            {                  "prefill_duration_us",                          observation.prefill_duration_us },
            {            "prefill_device_node_count",                    observation.prefill.device_node_count },
            {           "prefill_device_duration_us",                   observation.prefill.device_duration_us },
            {           "prefill_compute_node_count",                   observation.prefill.compute_node_count },
            {          "prefill_compute_duration_us",                  observation.prefill.compute_duration_us },
            {            "prefill_kernel_node_count",                                     prefill_kernel_count },
            {           "prefill_kernel_duration_us",                   observation.prefill.kernel_duration_us },
            {     "prefill_common_kernel_node_count",        observation.prefill_common_kernel_node_ids.size() },
            {    "prefill_common_kernel_duration_us",            observation.prefill_common_kernel_duration_us },
            {  "prefill_prefix_attention_node_count",     observation.prefill_prefix_attention_node_ids.size() },
            { "prefill_prefix_attention_duration_us",         observation.prefill_prefix_attention_duration_us },
            {        "prefill_collective_node_count",           observation.prefill.collective_node_ids.size() },
            {       "prefill_collective_duration_us",               observation.prefill.collective_duration_us },
            {              "prefill_kernel_families", phase_cost_families(observation.prefill.kernel_families) },
            {        "prefill_submit_cpu_node_count",           observation.prefill.submit_cpu_node_ids.size() },
            {       "prefill_submit_cpu_duration_us",               observation.prefill.submit_cpu_duration_us },
            {               "decode_iteration_count",                       observation.decode_iteration_count },
            {                   "decode_duration_us",                           observation.decode_duration_us },
            {             "decode_device_node_count",                     observation.decode.device_node_count },
            {            "decode_device_duration_us",                    observation.decode.device_duration_us },
            {            "decode_compute_node_count",                    observation.decode.compute_node_count },
            {           "decode_compute_duration_us",                   observation.decode.compute_duration_us },
            {             "decode_kernel_node_count",                observation.decode_kernel_node_ids.size() },
            {            "decode_kernel_duration_us",                    observation.decode.kernel_duration_us },
            {         "decode_collective_node_count",            observation.decode.collective_node_ids.size() },
            {        "decode_collective_duration_us",                observation.decode.collective_duration_us },
            {               "decode_kernel_families",  phase_cost_families(observation.decode.kernel_families) },
            {         "decode_submit_cpu_node_count",            observation.decode.submit_cpu_node_ids.size() },
            {        "decode_submit_cpu_duration_us",                observation.decode.submit_cpu_duration_us },
        });
    }
    return Json{
        {                            "status",                                       phase.status },
        {           "cache_extend_fact_count",                      phase.cache_extend_fact_count },
        {          "request_bound_fact_count",                     phase.request_bound_fact_count },
        {              "paired_prefill_count",                         phase.paired_prefill_count },
        {               "paired_decode_count",                          phase.paired_decode_count },
        {    "unmatched_prefill_marker_count",               phase.unmatched_prefill_marker_count },
        {     "unmatched_decode_marker_count",                phase.unmatched_decode_marker_count },
        {                "invalid_fact_count",                           phase.invalid_fact_count },
        {           "token_range_error_count",                      phase.token_range_error_count },
        {     "phase_owned_device_node_count",                phase.phase_owned_device_node_count },
        { "phase_owned_submit_cpu_node_count",            phase.phase_owned_submit_cpu_node_count },
        {                 "layer_wait_status",                            phase.layer_wait_status },
        {                 "layer_wait_issues",                            phase.layer_wait_issues },
        {        "phase_owner_conflict_count",                   phase.phase_owner_conflict_count },
        {           "prefill_device_families", phase_cost_families(phase.prefill_device_families) },
        {            "decode_device_families",  phase_cost_families(phase.decode_device_families) },
        {       "decode_iterations_histogram",                  phase.decode_iterations_histogram },
        {                      "observations",                              std::move(phase_rows) },
    };
}


nlohmann::json hicache_io_observations(const core::DagGraph & graph, const modules::hicache::patch::HiCacheIoOperationLedger & operations,
                                       bool cpu_service_applied) {
    using modules::hicache::patch::hicache_io_operation_kind_name;
    using modules::hicache::patch::HiCacheIoOperationKind;
    using Json = nlohmann::json;

    Json rows = Json::array();
    for (const auto & item : operations.records) {
        const bool dma = item.kind == HiCacheIoOperationKind::Load || item.kind == HiCacheIoOperationKind::WriteDeviceToHost;
        const auto service_us = dma ? item.device_transfer_duration_us : item.observed_service_duration_us;
        const bool service_observed = dma ? !item.device_transfer_node_ids.empty() : !item.storage_service_batches.empty();
        uint64_t admission_us = 0;
        for (const auto node_id : item.admission_explicit_node_ids) {
            admission_us = core::checked_add_u64(admission_us, graph.cpu_service_node_duration(node_id), "I/O observation admission duration overflow");
        }
        Json service_batches = Json::array();
        uint64_t service_pages = dma && service_observed && item.source_page_size > 0 ? item.completed_token_count / item.source_page_size : 0;
        for (const auto & batch : item.storage_service_batches) {
            service_pages = core::checked_add_u64(service_pages, batch.item_count, "I/O service page count overflow");
            service_batches.push_back(Json{
                {         "timing_fact_node_id",                                                             batch.fact_node_id },
                {                    "start_us",                                                                 batch.start_us },
                {                  "service_us",                                                              batch.duration_us },
                {                  "page_count",                                                               batch.item_count },
                {           "copied_page_count",       batch.copied_page_count ? Json(*batch.copied_page_count) : Json(nullptr) },
                {        "published_page_count", batch.published_page_count ? Json(*batch.published_page_count) : Json(nullptr) },
                { "storage_existing_page_count",   batch.existing_page_count ? Json(*batch.existing_page_count) : Json(nullptr) },
                {      "storage_new_page_count",             batch.new_page_count ? Json(*batch.new_page_count) : Json(nullptr) },
            });
        }
        rows.push_back(Json{
            {                        "record_id",                                                                                     item.record_id },
            {                  "source_start_us",                                                                               item.source_start_us },
            {              "timing_fact_node_id",                                                                           item.timing_fact_node_id },
            {                             "kind",                                                          hicache_io_operation_kind_name(item.kind) },
            {                        "direction",                                                                                     item.direction },
            {                           "status",                                                                                        item.status },
            {                           "reason",                                                                                        item.reason },
            {                   "resource_scope",                                                                                   item.cache_scope },
            {                              "pid",                                                                                           item.pid },
            {                       "request_id",                                                                                    item.request_id },
            {                     "operation_id",                                                                                  item.operation_id },
            {                  "operation_count",                                                                                                  1 },
            {                 "completed_tokens",                                                                         item.completed_token_count },
            // Storage calls may execute even when cancellation publishes no tokens.
            {               "service_page_count",                                                                                      service_pages },
            {                        "page_size",                                                                              item.source_page_size },
            {                       "service_us",                                                                                         service_us },
            {                    "service_clock",                                                          dma ? "device_transfer" : "function_wall" },
            {                 "service_observed",                                                                                   service_observed },
            {          "storage_service_batches",                                                                         std::move(service_batches) },
            {             "admission_control_us",                                                                                       admission_us },
            {       "admission_control_observed",                                                          !item.admission_explicit_node_ids.empty() },
            {                 "terminal_span_us",                                                                  item.retained_terminal_control_us },
            {         "terminal_explicit_cpu_us",                                                                           item.terminal_control_us },
            { "terminal_explicit_cpu_node_count",                                                              item.terminal_control_node_ids.size() },
            {        "terminal_control_observed",                     item.completion_join_contract_ready && !item.terminal_control_node_ids.empty() },
            {  "host_available_tokens_at_return", item.host_available_tokens_at_return ? Json(*item.host_available_tokens_at_return) : Json(nullptr) },
            {       "storage_residency_observed",                                                                    item.storage_residency_observed },
            {      "storage_existing_page_count",                                                                   item.storage_existing_page_count },
            {           "storage_new_page_count",                                                                        item.storage_new_page_count },
        });
    }
    return Json{
        {     "cpu_cost_basis", cpu_service_applied ? "source_bound_service" : "profiled" },
        {             "status",                                         operations.status },
        {       "record_count",                                 operations.records.size() },
        { "unresolved_reasons",                             operations.unresolved_reasons },
        {       "observations",                                           std::move(rows) },
    };
}

} // namespace markov::trace_graph::cli
