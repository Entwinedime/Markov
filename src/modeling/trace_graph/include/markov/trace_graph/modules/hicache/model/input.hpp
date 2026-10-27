#pragma once

#include "markov/trace_graph/modules/hicache/router.hpp"

namespace markov::trace_graph::modules::hicache::model {

/** Source identity and execution status for one approved state input.
 * Observers use fact/role; the target replay alone advances started/consumed.
 */
struct HiCacheReplayFact {
    HiCacheFact fact;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
    bool consumed = false;
    bool started = false;
};

} // namespace markov::trace_graph::modules::hicache::model
