/**
 * @file
 * @brief Source/target rewrite classification and read-only shadow planning.
 */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"
#include "markov/trace_graph/modules/hicache/patch/attribution.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch {

/** @brief Semantic graph operation selected from one source/target effect matrix row. */
enum class HiCacheRewriteKind : std::uint8_t {
    NoOp,
    ReplaceWithIo,
    ReplaceWithGate,
    RemoveOwnedCost,
    RemoveDependency,
    InsertIo,
    InsertGate,
    PartialReplace,
    Reject,
};

/** @brief One explicit rewrite decision, including why shadow planning may be blocked. */
struct HiCacheRewriteDecision {
    std::string effect_id;
    std::string effect_family_id;
    std::string cache_scope;
    std::string request_id;
    uint64_t eligibility_timestamp_us = 0;
    model::HiCacheEffectType effect_type = model::HiCacheEffectType::Loadback;
    model::HiCacheTargetEffectState target_effect_state = model::HiCacheTargetEffectState::Unresolved;
    model::HiCacheSourceCarrierState source_carrier_state = model::HiCacheSourceCarrierState::NotEvaluated;
    HiCacheRewriteKind rewrite_kind = HiCacheRewriteKind::Reject;
    bool shadow_plan_ready = false;
    uint64_t duration_us = 0;
    std::string resource_lane;
    std::string synthetic_id;
    std::vector<size_t> carrier_nodes;
    std::vector<size_t> owned_duration_nodes;
    std::vector<HiCacheCpuGapSlice> owned_gap_slices;
    std::vector<size_t> source_control_duration_nodes;
    std::vector<HiCacheCpuGapSlice> source_control_gap_slices;
    std::vector<HiCacheCpuGapSlice> source_gap_removal_slices;
    std::vector<HiCacheCpuGapSlice> logical_input_causal_gap_slices;
    uint64_t observed_io_duration_us = 0;
    uint64_t owned_gap_duration_us = 0;
    uint64_t target_host_control_duration_us = 0;
    bool target_host_control_required = false;
    bool target_host_control_terminal = false;
    std::string target_host_control_synthetic_id;
    std::string target_host_control_terminal_join_synthetic_id;
    std::optional<size_t> target_host_control_anchor_node_id = std::nullopt;
    std::optional<size_t> target_host_control_exit_node_id = std::nullopt;
    std::vector<size_t> target_host_control_exit_node_ids;
    std::optional<size_t> target_host_control_ingress_edge_id = std::nullopt;
    std::vector<size_t> target_host_control_ingress_edge_ids;
    uint64_t source_gap_removal_duration_us = 0;
    uint64_t logical_input_causal_gap_duration_us = 0;
    uint64_t residual_unknown_duration_us = 0;
    std::string observed_span_semantics = "unknown";
    std::string completion_wait_status = "not_applicable";
    std::string completion_wait_reason;
    bool completion_join_contract_ready = false;
    bool completion_join_required = false;
    bool source_effect_schedule_aligned = true;
    bool source_readiness_topology_reused = false;
    bool source_completion_wait_blocking = false;
    bool source_control_removal_required = false;
    std::string completion_join_synthetic_id;
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
    std::optional<size_t> completion_control_ingress_edge_id = std::nullopt;
    std::vector<size_t> completion_wait_owned_node_ids;
    std::vector<size_t> source_completion_node_ids;
    std::vector<size_t> readiness_join_node_ids;
    std::vector<HiCacheCpuGapSlice> completion_wait_slices;
    std::vector<HiCacheCpuGapSlice> logical_input_completion_wait_slices;
    std::vector<size_t> carrier_entry_edges;
    std::vector<size_t> carrier_exit_edges;
    /** @brief Semantic source-fact identity retained for diagnostics. */
    size_t source_fact_node_id = 0;
    /** @brief Proven executable launch anchor required for synthetic insertion. */
    std::optional<size_t> source_execution_anchor_node_id = std::nullopt;
    std::vector<size_t> consumer_anchors;
    std::string consumer_anchor_method;
    std::string request_consumer_synthetic_id;
    std::string family_consumer_synthetic_id;
    std::string reason;
    std::string blocker;
};

/** @brief Complete read-only transaction candidate for one prediction cell. */
struct HiCacheShadowRewriteTransaction {
    std::string status = "not_built";
    std::vector<HiCacheRewriteDecision> decisions;
    core::DagMutationPlan plan;
    bool topology_valid = false;
    size_t prospective_active_node_count = 0;
    size_t prospective_active_edge_count = 0;
    std::map<std::string, uint64_t> counts_by_rewrite_kind;
    std::map<std::string, uint64_t> blocker_counts;
    std::map<std::string, std::vector<std::string>> ownership_conflicts;
#ifdef DEBUG
    core::DagTopologyValidationReport topology;
#endif

    [[nodiscard]] uint64_t ready_count() const;
    [[nodiscard]] uint64_t rejected_count() const;
};

/** @brief Returns a stable artifact name for one rewrite kind. */
[[nodiscard]] std::string hicache_rewrite_kind_name(HiCacheRewriteKind kind);

/**
 * Replace only the global fixed Load-control component with a source-carrier
 * intrinsic fact when a source dirty retry becomes a predicted target no-retry.
 */
/**
 * @brief Classifies every effect and validates a prospective plan without applying it.
 *
 * The returned plan is diagnostic only. The caller must not apply it until every
 * supported effect, boundary, ownership check, and shape gate is complete.
 */
[[nodiscard]] HiCacheShadowRewriteTransaction build_hicache_shadow_rewrite_transaction(const core::DagGraph & graph,
                                                                                       const model::HiCacheEffectDecisionLedger & effects,
                                                                                       const HiCacheSourceAttributionCatalog & attributions,
                                                                                       const HiCacheIoResourcePlan & resources,
                                                                                       bool source_target_same_config = false);

} // namespace markov::trace_graph::modules::hicache::patch
