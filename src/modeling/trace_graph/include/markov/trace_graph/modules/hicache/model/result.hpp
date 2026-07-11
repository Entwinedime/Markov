/**
 * @file
 * @brief Business result and Debug-summary boundary for HiCache state replay.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_intent.hpp"
#include "markov/trace_graph/modules/hicache/model/summary.hpp"

namespace markov::trace_graph::modules::hicache::model {

struct HiCacheModelResult {
    bool replay_complete = false;
    HiCacheEffectIntentCatalog effect_intents;
#ifdef DEBUG
    HiCacheSummary summary;
#endif
};

} // namespace markov::trace_graph::modules::hicache::model
