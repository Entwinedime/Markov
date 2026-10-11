#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {
struct HiCacheReplayFact;
}

namespace markov::trace_graph::source_dag {

/** Source admission opportunity, independent of whether source needed loading.
 * owner is the preceding same-request lookup fact. Envelope and condition
 * locate the CPU branch; target cache state determines whether it loads.
 */
struct HiCacheLoadBranch {
    size_t owner;
    core::TraceEvent envelope, condition;
    size_t entry_node = 0, return_node = 0;
};

[[nodiscard]] std::vector<HiCacheLoadBranch> observe_load_branches(std::span<const core::TraceEvent> observations,
                                                                   std::span<const modules::hicache::model::HiCacheReplayFact> facts, uint64_t begin_us,
                                                                   uint64_t end_us);

/** Prelude eligibility from completed Python checks. Torch envelopes are not
 * required before profiling starts; no check outcome is used as a target hit.
 * Returns preceding lookup identities, rejecting incomplete or ambiguous input.
 */
[[nodiscard]] std::vector<size_t> observe_prelude_load_lookups(std::span<const core::TraceEvent> observations,
                                                               std::span<const modules::hicache::model::HiCacheReplayFact> facts);

} // namespace markov::trace_graph::source_dag
