/**
 * @file
 * @brief Request-bound source observations for SGLang prefill and decode phases.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::core {
class DagGraph;
}

namespace markov::trace_graph::modules::hicache {

struct HiCacheLayerWaitObservation;

struct HiCachePhaseCostFamily {
    size_t node_count = 0;
    uint64_t duration_us = 0;
};

/** @brief Device work and correlated CPU submission costs for either phase. */
struct HiCachePhaseNodeObservation {
    size_t device_node_count = 0;
    uint64_t device_duration_us = 0;
    size_t compute_node_count = 0;
    uint64_t compute_duration_us = 0;
    uint64_t kernel_duration_us = 0;
    uint64_t collective_duration_us = 0;
    std::vector<size_t> collective_node_ids;
    std::vector<size_t> submit_cpu_node_ids;
    std::map<std::string, HiCachePhaseCostFamily> kernel_families;
    uint64_t submit_cpu_duration_us = 0;
};

/** @brief One cache-extend batch joined to its Torch execution markers. */
struct HiCachePhaseObservation {
    int logical_input = 0;
    std::string pid;
    std::vector<std::string> request_ids;
    uint64_t batch_size = 0;
    uint64_t source_page_size = 0;
    uint64_t prompt_token_count = 0;
    uint64_t prefill_token_count = 0;
    uint64_t prefill_start_us = 0;
    uint64_t prefill_duration_us = 0;
    HiCachePhaseNodeObservation prefill;
    uint64_t prefill_common_kernel_duration_us = 0;
    uint64_t prefill_prefix_attention_duration_us = 0;
    std::vector<size_t> prefill_common_kernel_node_ids;
    std::vector<size_t> prefill_prefix_attention_node_ids;
    uint64_t decode_iteration_count = 0;
    uint64_t decode_duration_us = 0;
    HiCachePhaseNodeObservation decode;
    std::vector<size_t> decode_kernel_node_ids;
};

/** @brief Compact completeness audit and request-level phase observations. */
struct HiCachePhaseObservationAudit {
    std::string status = "not_ready";
    std::string layer_wait_status = "unavailable";
    std::map<std::string, size_t> layer_wait_issues;
    size_t cache_extend_fact_count = 0;
    size_t request_bound_fact_count = 0;
    size_t paired_prefill_count = 0;
    size_t paired_decode_count = 0;
    size_t unmatched_prefill_marker_count = 0;
    size_t unmatched_decode_marker_count = 0;
    size_t invalid_fact_count = 0;
    size_t token_range_error_count = 0;
    size_t phase_owned_device_node_count = 0;
    size_t phase_owned_submit_cpu_node_count = 0;
    size_t phase_owner_conflict_count = 0;
    std::map<std::string, HiCachePhaseCostFamily> prefill_device_families;
    std::map<std::string, HiCachePhaseCostFamily> decode_device_families;
    std::map<uint64_t, size_t> decode_iterations_histogram;
    std::vector<HiCachePhaseObservation> observations;

    [[nodiscard]] bool ready() const { return status == "ready"; }
};

/** @brief Share Decode attention classification between observation and patch. */
[[nodiscard]] bool is_hicache_paged_attention(std::string_view name);

/** @brief Sum the semantic paged-attention family in one Decode observation. */
[[nodiscard]] uint64_t hicache_paged_attention_duration(const std::map<std::string, HiCachePhaseCostFamily> & families);

/**
 * @brief Joins request-bearing cache-extend facts to Torch phase markers.
 *
 * The join is based on rank-local semantic order. It has no config/cell table and
 * does not use a fixed time tolerance. Marker intervals remain observations, not
 * ownership windows for Direct I/O or residual CPU gaps.
 */
[[nodiscard]] HiCachePhaseObservationAudit observe_hicache_phases(const core::DagGraph & graph, const HiCacheLayerWaitObservation * layer_waits = nullptr);

} // namespace markov::trace_graph::modules::hicache
