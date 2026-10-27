#pragma once

#include "markov/trace_graph/source/build.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <optional>
#include <vector>

namespace markov::trace_graph::source {

enum class ServiceIssue { SharedProbe, AmbiguousOwner, UnmatchedDevice, NonServiceDevice };

struct DeviceService {
    // References retain source resources and exclusive intervals. The CPU probe
    // envelope locates submissions; its wall duration is never a device cost.
    std::vector<graph::NodeId> nodes;
    std::optional<ServiceIssue> issue;

    bool available() const { return !nodes.empty() && !issue; }
};

// Bind only observed native submissions with a unique enclosing work call and
// an exact device start port. No nearest-time or kernel-name matching is used.
std::vector<DeviceService> bind_device_services(const Observations & observations, const SourceGraph & graph);

} // namespace markov::trace_graph::source
