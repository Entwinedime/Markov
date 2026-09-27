#pragma once
#include "markov/trace_graph/modules/hicache/model/replay.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

struct HiCacheLifecycleInsert {
    size_t owner;
    core::TraceEvent envelope;
    uint64_t path_tokens;
    std::vector<core::TraceEvent> node_boundaries;
};

struct HiCacheLifecycleStep {
    uint64_t begin_tokens, end_tokens;
    core::TraceEvent ready;
};

/** Ready-to-apply milestones, not costs or target cache decisions. */
[[nodiscard]] std::vector<HiCacheLifecycleStep> lifecycle_ready_steps(const HiCacheLifecycleInsert & insert);

/** Source-only ownership and path audit. Missing observations are not inferred
 * from nearby calls; node identities are local to this source, not the target. */
[[nodiscard]] std::vector<HiCacheLifecycleInsert> observe_lifecycle_inserts(
    const core::DagGraph & graph, std::span<const model::HiCacheReplayFact> facts, uint64_t begin, uint64_t end);

} // namespace markov::trace_graph::modules::hicache::runtime
