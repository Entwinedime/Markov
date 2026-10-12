#pragma once

#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <utility>

namespace markov::trace_graph::frontend {
struct HiCacheIoResourceLanesConfig;
}

namespace markov::trace_graph::structure::hicache {

using SchedulerComputeResources = std::map<std::pair<std::string, std::string>, size_t>;

struct SchedulerDeviceResources {
    SchedulerComputeResources schedule;
    SchedulerComputeResources forward;
};

/** Select the declared execution resource, independent of service coefficients.
 * Storage can serialize across scopes; DMA remains local to its cache scope.
 */
[[nodiscard]] std::string io_resource_lane(const frontend::HiCacheIoResourceLanesConfig & lanes, const std::string & kind, const std::string & scope);

/** Separate scheduler-preparation and forward stream identities. Allocator
 * submissions identify schedule work; layer checks identify forward work.
 * Observe before replacing source calls. Values are source graph node handles,
 * not costs or target work templates. Missing/ambiguous forward placements are
 * not fabricated; conflicting proven identities on one scheduler fail clearly.
 */
[[nodiscard]] SchedulerDeviceResources observe_scheduler_compute_resources(const modules::hicache::patch::HiCacheSourceDagIndex & source);

} // namespace markov::trace_graph::structure::hicache
