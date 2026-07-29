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
    std::vector<size_t> carrier_entry_edges;
    std::vector<size_t> carrier_exit_edges;
    /** @brief Semantic source-fact identity retained for diagnostics. */
    size_t source_fact_node_id = 0;
    /** @brief Proven executable launch anchor required for synthetic insertion. */
    std::optional<size_t> source_execution_anchor_node_id = std::nullopt;
    std::vector<size_t> consumer_anchors;
    std::string consumer_anchor_method;
    std::string reason;
    std::string blocker;
};

/** @brief Complete read-only transaction candidate for one prediction cell. */
struct HiCacheShadowRewriteTransaction {
    std::string status = "not_built";
    std::string io_model_calibration_status;
    bool io_model_allows_apply = false;
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
 * @brief Classifies every effect and validates a prospective plan without applying it.
 *
 * The returned plan is diagnostic only. The caller must not apply it until every
 * supported effect, boundary, ownership check, and shape gate is complete.
 */
[[nodiscard]] HiCacheShadowRewriteTransaction build_hicache_shadow_rewrite_transaction(const core::DagGraph & graph,
                                                                                       const model::HiCacheEffectDecisionLedger & effects,
                                                                                       const HiCacheSourceAttributionCatalog & attributions,
                                                                                       const HiCacheIoResourcePlan & resources);

} // namespace markov::trace_graph::modules::hicache::patch
