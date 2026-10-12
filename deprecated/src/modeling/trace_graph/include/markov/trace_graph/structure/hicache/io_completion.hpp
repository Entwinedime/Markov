#pragma once

#include "markov/trace_graph/structure/hicache/prefetch.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace markov::trace_graph::structure::hicache {

/** Completion timings supplied by execution, not coefficients visible to rules.
 * All coordinates and durations are microseconds. Positive work without a
 * timing model throws MissingCostEvidence rather than disappearing from state.
 */
struct IoCompletionProvider {
    // direction/family, bytes per page, page count, existing-page count -> us.
    std::function<uint64_t(const std::string &, uint64_t, uint64_t, uint64_t)> duration;
    // bytes per page, page count, absolute start_us -> completion coordinates.
    std::function<modules::hicache::PrefetchControlBatch(uint64_t, uint64_t, uint64_t)> prefetch_batch;
};

} // namespace markov::trace_graph::structure::hicache
