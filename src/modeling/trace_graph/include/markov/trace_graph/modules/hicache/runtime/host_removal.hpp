#pragma once
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"

#include <string>

namespace markov::trace_graph::modules::hicache::runtime {

struct HiCacheHostRegion {
    size_t entry, exit;
    const HiCacheHostTemplate * host;
};

/** Plan a zero-cost entry before existing CPU work, retaining that work and its
 * outgoing gap. The source index must describe the graph before the whole plan.
 * Resolve the returned symbolic node only after applying the plan. */
[[nodiscard]] core::DagNodeRef append_host_entry_gate(const patch::HiCacheSourceDagIndex & source, size_t first, core::DagObservedPoint point, std::string id,
                                                      std::string name, core::DagMutationPlan & plan);

/** Remove complete observed calls before simulation. Entry/exit gates survive
 * for target insertion. Event consumers must belong to the removed region;
 * surviving stream synchronizations retain the preceding live stream work.
 * Planning does not modify the graph or assign any replacement cost. */
[[nodiscard]] core::DagMutationPlan plan_host_removal(const patch::HiCacheSourceDagIndex & source, std::span<const HiCacheHostRegion> regions);

} // namespace markov::trace_graph::modules::hicache::runtime
