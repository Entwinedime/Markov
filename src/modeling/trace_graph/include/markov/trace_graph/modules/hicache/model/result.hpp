/**
 * @file
 * @brief Business result and Debug-summary boundary for HiCache state replay.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"

namespace markov::trace_graph::modules::hicache::model {

struct HiCacheModelResult {
    bool replay_complete = false;
    HiCacheEffectDecisionLedger effect_decisions;
    /** Numerical cost fields remain separate from target-derived effects. */
    frontend::HiCacheIoCostConfig io_cost_model;
};

} // namespace markov::trace_graph::modules::hicache::model
