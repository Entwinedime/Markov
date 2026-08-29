/**
 * @file
 * @brief Conservative source-effect attribution over an indexed active DAG.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch {

/** @brief Evidence and owned graph atoms for one stable target effect decision. */
struct HiCacheSourceAttribution {
    std::string effect_id;
    model::HiCacheEffectType effect_type = model::HiCacheEffectType::Loadback;
    model::HiCacheTargetEffectState target_effect_state = model::HiCacheTargetEffectState::Unresolved;
    model::HiCacheSourceCarrierState source_carrier_state = model::HiCacheSourceCarrierState::NotEvaluated;
    size_t source_fact_node_id = 0;
    std::optional<size_t> source_execution_anchor_node_id = std::nullopt;
    std::vector<std::string> evidence;
    std::vector<size_t> control_fact_nodes;
    std::vector<size_t> timing_fact_nodes;
    std::vector<size_t> operation_chain_nodes;
    std::vector<std::string> io_operation_record_ids;
    std::vector<size_t> carrier_nodes;
    std::vector<size_t> owned_duration_nodes;
    std::vector<HiCacheCpuGapSlice> owned_gap_slices;
    std::vector<size_t> source_control_duration_nodes;
    std::vector<HiCacheCpuGapSlice> source_control_gap_slices;
    std::vector<HiCacheCpuGapSlice> source_gap_removal_slices;
    std::vector<HiCacheCpuGapSlice> logical_input_causal_gap_slices;
    uint64_t source_completed_token_count = 0;
    uint64_t target_effective_token_count = 0;
    uint64_t observed_io_duration_us = 0;
    uint64_t owned_gap_duration_us = 0;
    uint64_t source_control_duration_us = 0;
    uint64_t source_control_gap_duration_us = 0;
    uint64_t source_gap_removal_duration_us = 0;
    uint64_t logical_input_causal_gap_duration_us = 0;
    uint64_t residual_unknown_duration_us = 0;
    std::string observed_span_semantics = "unknown";
    std::string completion_wait_status = "not_applicable";
    std::string completion_wait_reason;
    bool completion_join_contract_ready = false;
    bool source_readiness_topology_ready = false;
    bool source_effect_schedule_aligned = true;
    bool source_completion_wait_blocking = false;
    bool source_control_removal_required = false;
    uint64_t control_ready_us = 0;
    uint64_t source_completion_us = 0;
    uint64_t wait_exit_start_us = 0;
    uint64_t wait_exit_end_us = 0;
    uint64_t completion_wait_duration_us = 0;
    uint64_t completion_wait_gap_duration_us = 0;
    uint64_t logical_input_completion_wait_duration_us = 0;
    uint64_t polling_lag_us = 0;
    uint64_t retained_terminal_control_us = 0;
    std::optional<size_t> control_ready_anchor_node_id = std::nullopt;
    std::optional<size_t> wait_exit_anchor_node_id = std::nullopt;
    std::optional<size_t> terminal_control_anchor_node_id = std::nullopt;
    std::vector<size_t> completion_wait_owned_node_ids;
    std::vector<size_t> source_completion_node_ids;
    std::vector<size_t> readiness_join_node_ids;
    std::vector<HiCacheCpuGapSlice> completion_wait_slices;
    std::vector<HiCacheCpuGapSlice> logical_input_completion_wait_slices;
    std::vector<size_t> carrier_entry_edges;
    std::vector<size_t> carrier_exit_edges;
    std::optional<size_t> start_anchor = std::nullopt;
    std::optional<size_t> completion_anchor = std::nullopt;
    std::vector<size_t> consumer_anchors;
    std::string consumer_anchor_method;
    std::string reason;
};

/** @brief Cell-wide attribution inventory; graph mutation remains disabled. */
struct HiCacheSourceAttributionCatalog {
    std::string status = "not_built";
    std::vector<HiCacheSourceAttribution> records;
    std::map<std::string, uint64_t> counts_by_source_carrier_state;
    std::map<std::string, uint64_t> counts_by_effect_type;
    std::map<std::string, uint64_t> blocker_counts;
    uint64_t d2h_ready_record_count = 0;
    uint64_t d2h_claimed_record_count = 0;
    uint64_t d2h_unclaimed_record_count = 0;
    uint64_t d2h_multiply_claimed_record_count = 0;

    [[nodiscard]] uint64_t attributed_count() const;
    [[nodiscard]] uint64_t unresolved_count() const;
};

/** @brief Builds a read-only attribution inventory without modifying target decisions. */
[[nodiscard]] HiCacheSourceAttributionCatalog build_hicache_source_attribution(const HiCacheSourceDagIndex & source,
                                                                               const model::HiCacheEffectDecisionLedger & decisions,
                                                                               const HiCacheIoOperationLedger & operations);

} // namespace markov::trace_graph::modules::hicache::patch
