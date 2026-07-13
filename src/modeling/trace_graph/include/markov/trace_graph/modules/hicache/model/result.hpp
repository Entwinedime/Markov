/**
 * @file
 * @brief Business result and Debug-summary boundary for HiCache state replay.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"
#include "markov/trace_graph/modules/hicache/model/summary.hpp"

namespace markov::trace_graph::modules::hicache::model {

struct HiCacheModelResult {
    bool replay_complete = false;
    HiCacheEffectDecisionLedger effect_decisions;
#ifdef DEBUG
    HiCacheSummary summary;
#endif
};

} // namespace markov::trace_graph::modules::hicache::model
