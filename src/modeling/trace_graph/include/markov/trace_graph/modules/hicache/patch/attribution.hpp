/**
 * @file
 * @brief Conservative source-effect attribution over an indexed active DAG.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"
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
    std::string identity_method;
    std::vector<std::string> evidence;
    std::vector<size_t> control_fact_nodes;
    std::vector<size_t> timing_fact_nodes;
    std::vector<size_t> operation_chain_nodes;
    std::vector<size_t> carrier_nodes;
    std::vector<size_t> owned_duration_nodes;
    std::vector<size_t> carrier_internal_edges;
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

    [[nodiscard]] uint64_t attributed_count() const;
    [[nodiscard]] uint64_t unresolved_count() const;
};

/** @brief Builds a read-only attribution inventory without modifying target decisions. */
[[nodiscard]] HiCacheSourceAttributionCatalog build_hicache_source_attribution(const HiCacheSourceDagIndex & source,
                                                                               const model::HiCacheEffectDecisionLedger & decisions);

} // namespace markov::trace_graph::modules::hicache::patch
