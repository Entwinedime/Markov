#pragma once
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace markov::trace_graph::modules::hicache::runtime {

struct HiCacheHostRegion {
    size_t entry, exit;
    const HiCacheHostTemplate * host;
    // Work already submitted by this same call before the retained CPU entry.
    // The caller proves ownership from original enqueue/native submissions;
    // these active source nodes are removed, never copied into a cost donor.
    std::span<const size_t> opening_nodes{};
};

/** Plan a zero-cost entry before existing CPU work, retaining that work and its
 * outgoing gap. The source index must describe the graph before the whole plan.
 * Resolve the returned symbolic node only after applying the plan. */
[[nodiscard]] core::DagNodeRef append_host_entry_gate(const patch::HiCacheSourceDagIndex & source, size_t first, core::DagObservedPoint point, std::string id,
                                                      std::string name, core::DagMutationPlan & plan);

/** Remove complete observed calls before simulation. Entry/exit gates survive
 * for target insertion. Event consumers must belong to the removed region;
 * surviving stream synchronizations retain the preceding live stream work.
 * collective_handoffs names exact incoming HCCL edges whose live completion
 * source is explicitly retained by the caller's replacement expansion.
 * submission_handoffs redirects declared caller-to-worker Correlation edges
 * to new submission boundaries in the enclosing mutation plan. Those tasks
 * remain outside removal; the supplied host regions describe caller work only.
 * Planning does not modify the graph or assign any replacement cost. */
[[nodiscard]] core::DagMutationPlan plan_host_removal(const patch::HiCacheSourceDagIndex & source, std::span<const HiCacheHostRegion> regions,
                                                      std::span<const size_t> collective_handoffs = {},
                                                      std::span<const core::DagRedirectEdgeMutation> submission_handoffs = {});

} // namespace markov::trace_graph::modules::hicache::runtime
