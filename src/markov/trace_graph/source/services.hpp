#pragma once

#include "markov/trace_graph/planning/native_calls.hpp"
#include "markov/trace_graph/source/build.hpp"
#include "markov/trace_graph/source/cpu_costs.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace markov::trace_graph::source {

enum class ServiceIssue { SharedProbe, AmbiguousOwner, UnmatchedDevice, NonServiceDevice };

struct DeviceService {
    // References retain source resources and exclusive intervals. The CPU probe
    // envelope locates submissions; its wall duration is never a device cost.
    std::vector<graph::NodeId> nodes;
    std::optional<ServiceIssue> issue;
    std::optional<Index> resource;

    bool available() const { return !nodes.empty() && !issue; }
};

// Bind only observed native submissions with a unique enclosing work call and
// an exact device start port. No nearest-time or kernel-name matching is used.
std::vector<DeviceService> bind_device_services(const Observations & observations, const SourceGraph & graph);

struct CpuInterval {
    Time service_ns = 0, wait_ns = 0, residual_ns = 0, instrumentation_ns = 0, unobserved_ns = 0;
    Time stream_wait_ns = 0;
    CpuCorrection service_correction, residual_correction, excluded_correction;
    void add(const CpuInterval & other);
};

// Slice the source's exclusive CPU partition. Parent wall intervals and nested
// children are never added together; original profiled costs stay uncorrected.
// Optional estimates record coverage separately and exclude known waits/overhead.
std::vector<CpuInterval> cpu_intervals(const Observations & observations, const SourceGraph & graph, std::span<const ObservedRegion> regions,
                                       const CpuCosts * costs = nullptr);

struct CallBinding {
    Index caller;
    std::set<Index> workers, devices;
    std::optional<planning::CallResources> unique_resources() const;
};

struct NativeSequenceBinding {
    std::vector<Index> calls;
    std::optional<std::string> issue = "native_queue_identity_unavailable";
    std::vector<planning::NativeCall> definitions;
};

enum class NativeTiming { Cpu, ReadyDispatch, DeviceControl, DeviceService };

struct NativeWorkRegion {
    planning::Work work;
    ObservedRegion region;
    bool stream_wait_generated = false;
    NativeTiming timing = NativeTiming::Cpu;
    Time excluded_queue_wait_ns = 0;
    std::vector<ObservedRegion> excluded = {};
};

std::vector<CallBinding> call_resources(const Observations & observations, std::span<const ObservedRegion> regions);

} // namespace markov::trace_graph::source
