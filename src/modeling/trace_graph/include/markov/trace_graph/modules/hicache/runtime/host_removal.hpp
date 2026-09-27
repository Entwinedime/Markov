#pragma once
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

struct HiCacheHostRegion {
    size_t entry, exit;
    const HiCacheHostTemplate * host;
};

/** Remove complete observed calls before simulation. Entry/exit gates survive
 * for target insertion. Event consumers must belong to the removed region;
 * surviving stream synchronizations retain the preceding live stream work.
 * Planning does not modify the graph or assign any replacement cost. */
[[nodiscard]] core::DagMutationPlan plan_host_removal(const patch::HiCacheSourceDagIndex & source,
                                                     std::span<const HiCacheHostRegion> regions);

} // namespace markov::trace_graph::modules::hicache::runtime
