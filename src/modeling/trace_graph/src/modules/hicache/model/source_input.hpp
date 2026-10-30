#pragma once

#include "markov/trace_graph/modules/hicache/model/input.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::core {
class DagGraph;
}

namespace markov::trace_graph::modules::hicache::model {

/** Accepted base observations, before any target state or simulated clock. */
struct HiCacheSourceInput {
    HiCachePhaseObservationAudit phases;
    std::vector<HiCacheReplayFact> formal, prelude, prelude_decodes, decode_allocations;
    std::vector<size_t> prelude_lookups, formal_lookups;
    std::map<std::string, size_t> missing_facts;
};

HiCacheSourceInput observe_hicache_source_input(const core::DagGraph & graph);

} // namespace markov::trace_graph::modules::hicache::model
