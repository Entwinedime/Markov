#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace markov::trace_graph::frontend {
struct HiCachePhaseCostConfig;
}

namespace markov::trace_graph::modules::hicache::diagnostics {

struct StaticPhaseNodeCost {
    uint64_t source_duration_us = 0, predicted_duration_us = 0;
    std::vector<size_t> source_node_ids;
};

// These snapshots belong only to the retained static replay. Enrich a copy of
// structural work, never the state machine's work ledger or live prediction.
struct StaticPrefillCost : modules::hicache::model::HiCachePrefillWorkItem {
    StaticPhaseNodeCost common_kernel_cost, prefix_attention_cost, collective_cost, submit_cost;
    bool feature_covered = false;
};

struct StaticDecodeCost : modules::hicache::model::HiCacheDecodeWorkItem {
    uint64_t effective_page_count = 0;
    uint64_t source_paged_attention_duration_us = 0, predicted_paged_attention_duration_us = 0;
    StaticPhaseNodeCost kernel_cost, collective_cost, submit_cost;
    bool feature_covered = false;
};

struct StaticPhaseCosts {
    std::string status = "not_ready", cost_status = "disabled";
    std::vector<StaticPrefillCost> prefills;
    std::vector<StaticDecodeCost> decodes;
};

/** Source aggregate costs for static diagnostics only. The work argument is
 * read-only; live prediction instead binds individual operator models. */
[[nodiscard]] StaticPhaseCosts observed_static_phase_costs(const core::DagGraph & graph, const modules::hicache::model::HiCachePhaseWorkLedger & work);

/** Cost enrichment for the explicitly retained static diagnostic path only. */
[[nodiscard]] StaticPhaseCosts bind_static_phase_costs(const core::DagGraph & graph, const modules::hicache::model::HiCachePhaseWorkLedger & work,
                                                       const frontend::HiCachePhaseCostConfig & config);

} // namespace markov::trace_graph::modules::hicache::diagnostics
