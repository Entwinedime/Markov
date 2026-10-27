#pragma once


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

} // namespace markov::trace_graph::structure::hicache
