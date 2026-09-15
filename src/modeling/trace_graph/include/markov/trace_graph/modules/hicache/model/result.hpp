/**
 * @file
 * @brief Business result and Debug-summary boundary for HiCache state replay.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"

namespace markov::trace_graph::modules::hicache::model {

struct HiCacheModelResult {
    bool replay_complete = false;
    HiCacheEffectDecisionLedger effect_decisions;
    HiCachePhaseWorkLedger phase_work;
    /** Numerical cost fields remain separate from target-derived effects. */
    frontend::HiCacheIoCostConfig io_cost_model;
    frontend::HiCachePhaseCostConfig phase_cost_model;
};

} // namespace markov::trace_graph::modules::hicache::model
