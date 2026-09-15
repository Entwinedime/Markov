/**
 * @file
 * @brief Source-observed HiCache I/O operations and exact timing ownership.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch {

enum class HiCacheIoOperationKind : std::uint8_t { Prefetch, Load, WriteDeviceToHost, WriteHostToStorage };

/** One measured storage call; a logical I/O operation may contain several batches. */
struct HiCacheStorageServiceBatch {
    size_t fact_node_id = 0;
    uint64_t start_us = 0;
    uint64_t duration_us = 0;
    uint64_t item_count = 0;
    std::optional<uint64_t> existing_page_count;
    std::optional<uint64_t> new_page_count;
};

struct HiCacheIoOperationRecord {
    std::string record_id;
    HiCacheIoOperationKind kind = HiCacheIoOperationKind::Prefetch;
    std::string direction;
    std::string status = "unresolved";
    std::string request_id;
    std::string operation_id;
    std::vector<uint64_t> operation_node_ids;
    std::string cache_scope;
    std::string pid;
    std::string tid;
    uint64_t effective_token_count = 0;
    uint64_t completed_token_count = 0;
    bool completed_token_count_present = false;
    uint64_t source_page_size = 0;
    HiCacheTokenSpan full_path_span;
    uint64_t source_start_us = 0;
    uint64_t source_end_us = 0;
    uint64_t observed_duration_us = 0;
    std::string observed_span_semantics = "unknown";
    /** Ordered, non-overlapping calls; gaps between calls are not service time. */
    std::vector<HiCacheStorageServiceBatch> storage_service_batches;
    uint64_t observed_service_start_us = 0;
    uint64_t observed_service_duration_us = 0;
    bool storage_residency_observed = false;
    uint64_t storage_existing_page_count = 0;
    uint64_t storage_new_page_count = 0;
    uint64_t owned_node_duration_us = 0;
    uint64_t owned_gap_duration_us = 0;
    uint64_t overlapping_node_duration_us = 0;
    uint64_t max_node_overlap_us = 0;
    uint64_t uncovered_duration_us = 0;
    size_t timing_fact_node_id = 0;
    std::vector<size_t> control_fact_node_ids;
    std::vector<size_t> runtime_node_ids;
    std::vector<size_t> admission_explicit_node_ids;
    std::vector<size_t> admission_python_self_node_ids;
    std::vector<HiCacheCpuGapSlice> admission_cpu_gap_slices;
    std::vector<size_t> device_transfer_node_ids;
    std::vector<size_t> device_completion_node_ids;
    std::vector<size_t> readiness_join_node_ids;
    uint64_t device_transfer_duration_us = 0;
    bool source_readiness_topology_ready = false;
    std::vector<HiCacheCpuGapSlice> cpu_gap_slices;
    std::vector<HiCacheCpuOverlapSlice> cpu_overlap_slices;
    std::optional<size_t> source_anchor_node_id = std::nullopt;
    std::optional<size_t> completion_anchor_node_id = std::nullopt;
    std::optional<size_t> consumer_anchor_node_id = std::nullopt;
    std::string completion_wait_status = "not_applicable";
    std::string completion_wait_reason;
    bool completion_join_contract_ready = false;
    bool source_completion_wait_blocking = false;
    uint64_t control_ready_us = 0;
    uint64_t source_completion_us = 0;
    uint64_t wait_exit_start_us = 0;
    uint64_t wait_exit_end_us = 0;
    uint64_t completion_wait_duration_us = 0;
    uint64_t completion_wait_gap_duration_us = 0;
    uint64_t logical_input_completion_wait_duration_us = 0;
    uint64_t polling_lag_us = 0;
    uint64_t retained_terminal_control_us = 0;
    /** Retained explicit CPU work, excluding wrapper self, gaps and wait nodes. */
    uint64_t terminal_control_us = 0;
    /** Observed return-state, not target state or a positive-payload entry-state estimate. */
    std::optional<uint64_t> host_available_tokens_at_return;
    std::vector<size_t> terminal_control_node_ids;
    /** False checks observe the state-check primitive without terminal commit. */
    std::vector<uint64_t> progress_check_cpu_samples_us;
    std::optional<size_t> control_ready_anchor_node_id = std::nullopt;
    std::optional<size_t> wait_exit_anchor_node_id = std::nullopt;
    std::optional<size_t> terminal_control_anchor_node_id = std::nullopt;
    std::vector<size_t> completion_wait_owned_node_ids;
    std::vector<HiCacheCpuGapSlice> completion_wait_slices;
    std::vector<HiCacheCpuGapSlice> logical_input_completion_wait_slices;
    bool runtime_copy_observed = false;
    bool foreground_consumer_required = false;
    std::vector<std::string> evidence;
    std::string reason;
};

struct HiCacheIoOperationLedger {
    std::string status = "not_built";
    std::vector<HiCacheIoOperationRecord> records;
    std::map<std::string, uint64_t> counts_by_kind;
    std::map<std::string, uint64_t> counts_by_status;
    std::map<std::string, uint64_t> unresolved_reasons;

    [[nodiscard]] uint64_t ready_count() const;
    [[nodiscard]] uint64_t unresolved_count() const;
};

[[nodiscard]] bool hicache_io_operation_record_ready(const HiCacheIoOperationRecord & record);
[[nodiscard]] std::string hicache_io_operation_kind_name(HiCacheIoOperationKind kind);
[[nodiscard]] HiCacheIoOperationLedger build_hicache_io_operation_ledger(const HiCacheSourceDagIndex & source);

} // namespace markov::trace_graph::modules::hicache::patch
