/** @file Serializes the narrow source observations consumed by Python modeling. */
#include "hicache_observations.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <nlohmann/json.hpp>

namespace markov::trace_graph::cli {

nlohmann::json hicache_io_observations(
    const core::DagGraph & graph,
    const modules::hicache::patch::HiCacheIoOperationLedger & operations) {
    using modules::hicache::patch::HiCacheIoOperationKind;
    using modules::hicache::patch::hicache_io_operation_kind_name;
    using Json = nlohmann::json;

    Json rows = Json::array();
    for (const auto & item : operations.records) {
        const bool dma = item.kind == HiCacheIoOperationKind::Load
                         || item.kind == HiCacheIoOperationKind::WriteDeviceToHost;
        const auto service_us = dma ? item.device_transfer_duration_us : item.observed_service_duration_us;
        const bool service_observed = dma ? !item.device_transfer_node_ids.empty()
                                          : !item.storage_service_batches.empty();
        uint64_t admission_us = 0;
        for (const auto node_id : item.admission_explicit_node_ids) {
            admission_us = core::checked_add_u64(
                admission_us,
                graph.node(node_id).duration,
                "I/O observation admission duration overflow");
        }
        Json service_batches = Json::array();
        uint64_t service_pages = dma && service_observed && item.source_page_size > 0
                                     ? item.completed_token_count / item.source_page_size : 0;
        for (const auto & batch : item.storage_service_batches) {
            service_pages = core::checked_add_u64(service_pages, batch.item_count, "I/O service page count overflow");
            service_batches.push_back(Json{
                {"timing_fact_node_id", batch.fact_node_id},
                {"start_us", batch.start_us},
                {"service_us", batch.duration_us},
                {"page_count", batch.item_count},
                {"storage_existing_page_count",
                 batch.existing_page_count ? Json(*batch.existing_page_count) : Json(nullptr)},
                {"storage_new_page_count",
                 batch.new_page_count ? Json(*batch.new_page_count) : Json(nullptr)},
            });
        }
        rows.push_back(Json{
            {"record_id", item.record_id},
            {"source_start_us", item.source_start_us},
            {"timing_fact_node_id", item.timing_fact_node_id},
            {"kind", hicache_io_operation_kind_name(item.kind)},
            {"direction", item.direction},
            {"status", item.status},
            {"reason", item.reason},
            {"resource_scope", item.cache_scope},
            {"pid", item.pid},
            {"request_id", item.request_id},
            {"operation_id", item.operation_id},
            {"operation_count", 1},
            {"completed_tokens", item.completed_token_count},
            // Storage calls may execute even when cancellation publishes no tokens.
            {"service_page_count", service_pages},
            {"page_size", item.source_page_size},
            {"service_us", service_us},
            {"service_clock", dma ? "device_transfer" : "function_wall"},
            {"service_observed", service_observed},
            {"storage_service_batches", std::move(service_batches)},
            {"admission_control_us", admission_us},
            {"admission_control_observed", !item.admission_explicit_node_ids.empty()},
            {"terminal_span_us", item.retained_terminal_control_us},
            {"terminal_explicit_cpu_us", item.terminal_control_us},
            {"terminal_explicit_cpu_node_count", item.terminal_control_node_ids.size()},
            {"terminal_control_observed",
             item.completion_join_contract_ready && !item.terminal_control_node_ids.empty()},
            {"host_available_tokens_at_return",
             item.host_available_tokens_at_return ? Json(*item.host_available_tokens_at_return) : Json(nullptr)},
            {"progress_check_cpu_samples_us", item.progress_check_cpu_samples_us},
            {"storage_residency_observed", item.storage_residency_observed},
            {"storage_existing_page_count", item.storage_existing_page_count},
            {"storage_new_page_count", item.storage_new_page_count},
        });
    }
    return Json{
        {"status", operations.status},
        {"record_count", operations.records.size()},
        {"unresolved_reasons", operations.unresolved_reasons},
        {"observations", std::move(rows)},
    };
}

} // namespace markov::trace_graph::cli
