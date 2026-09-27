/** Dependency replay for observed synchronous Gloo/TCP collectives. */
#pragma once
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/cpu_collectives.hpp"

namespace markov::trace_graph::modules::hicache::patch {
struct CpuCollectiveWaitPlan {
    core::DagMutationPlan mutation{ .component = "cpu_collective" };
    std::map<std::string, size_t> issues;
    size_t calls = 0;
};

/** Observed post-arrival remainders, not calibrated communication service costs.
 * Broadcast uses entry lower bounds from the binomial tree, not per-hop timings.
 * All requested rounds are planned atomically; unsupported rounds are not skipped.
 */
[[nodiscard]] CpuCollectiveWaitPlan plan_cpu_collective_waits(const HiCacheSourceDagIndex & source, const std::vector<const CpuCollectiveRound *> & rounds);

struct CpuCollectivePatch {
    std::string status = "unavailable";
    size_t observed_rounds = 0, planned_rounds = 0;
    std::map<std::string, size_t> source_retained_rounds;
    std::map<std::string, size_t> blockers;
};

/** Append only whole rounds independent of the existing transaction. Intersecting
 * rounds remain source observations until their target loop is planned jointly.
 */
[[nodiscard]] CpuCollectivePatch append_retained_cpu_collectives(const HiCacheSourceDagIndex & source, const CpuCollectiveObservation & observation,
                                                                 core::DagMutationPlan & plan);
} // namespace markov::trace_graph::modules::hicache::patch
