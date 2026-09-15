/**
 * @file
 * @brief Target-derived Prefill/Decode work plans independent of phase cost.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

/** @brief Source carrier nodes and one predicted aggregate family duration. */
struct HiCachePhaseNodeCostPlan {
    uint64_t source_duration_us = 0;
    uint64_t predicted_duration_us = 0;
    std::vector<size_t> source_node_ids;
};

/** @brief One target prefill request projected at a cache-extend boundary. */
struct HiCachePrefillWorkItem {
    size_t source_fact_id = 0;
    size_t source_event_index = 0;
    int logical_input = -1;
    std::string pid;
    std::string request_id;
    uint64_t batch_position = 0;
    uint64_t batch_size = 0;
    uint64_t target_page_size = 0;
    uint64_t prompt_token_count = 0;
    uint64_t reusable_prefix_token_count = 0;
    uint64_t prefill_token_count = 0;
    uint64_t source_prefill_token_count = 0;
    double attention_token_pairs = 0.0;
    HiCachePhaseNodeCostPlan common_kernel_cost;
    HiCachePhaseNodeCostPlan prefix_attention_cost;
    HiCachePhaseNodeCostPlan kernel_cost;
    HiCachePhaseNodeCostPlan collective_cost;
    HiCachePhaseNodeCostPlan submit_cost;
    bool feature_covered = false;
};

/** @brief One target allocator call, including prelude calls that prepare runtime code. */
struct HiCacheAllocatorWorkItem {
    size_t source_fact_id = 0;
    size_t source_event_index = 0;
    std::string pid;
    std::vector<std::string> request_ids;
    bool formal = false;
    uint64_t page_size = 0;
    uint64_t batch_size = 0;
    uint64_t extend_tokens = 0;
    uint64_t allocated_pages = 0;
    std::optional<uint64_t> free_index_offset;
};

/** @brief Decode work inherited from the fixed source request contract. */
struct HiCacheDecodeWorkItem {
    int logical_input = -1;
    std::string pid;
    std::string request_id;
    uint64_t prompt_token_count = 0;
    uint64_t target_page_size = 0;
    uint64_t effective_page_count = 0;
    uint64_t iteration_count = 0;
    uint64_t source_paged_attention_duration_us = 0;
    uint64_t predicted_paged_attention_duration_us = 0;
    HiCachePhaseNodeCostPlan kernel_cost;
    HiCachePhaseNodeCostPlan collective_cost;
    HiCachePhaseNodeCostPlan submit_cost;
    bool feature_covered = false;
};

/** @brief Complete target phase-work prediction before any compute cost. */
struct HiCachePhaseWorkLedger {
    std::string status = "not_ready";
    std::string prefill_status = "not_ready";
    std::string decode_status = "not_ready";
    std::string cost_status = "disabled";
    std::vector<HiCachePrefillWorkItem> prefills;
    std::vector<HiCacheAllocatorWorkItem> allocator_calls;
    std::vector<HiCacheDecodeWorkItem> decodes;
    std::map<std::string, uint64_t> blockers;
};

} // namespace markov::trace_graph::modules::hicache::model
